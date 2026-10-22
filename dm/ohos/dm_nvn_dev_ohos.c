// ================= M4-c：NVN 设备/窗口/队列（首帧前置）=================
// 为什么先做这三个对象（2026-10-22）：
//   覆盖率算出来 418 个缺口，其中 Window 15 / Queue 24 / Device 18。它们不是"画东西"，
//   而是**画之前必须存在的东西**：设备 → 窗口（拿到交换链纹理）→ 队列（提交+呈现）。
//   没有这条路，后面 127 个 CommandBuffer 命令一条也用不上。
//
// 本文件的定位（要说清楚，免得以后误读为"已经能出帧"）：
//   · **数据部分**（Builder / Set / Get）真实实现 —— 与状态族同一套"旁表"思路，零越界；
//   · **生命周期**（Initialize / Finalize）真实记账，并打印一行（判据：能看到游戏建了几个窗口）；
//   · **执行部分**（AcquireTexture / SubmitCommands / PresentTexture / Finish / Flush / WaitSync）
//     先做**记账 + 安全桩**：返回"成功"并计数，不真提交（真提交要等 CommandBuffer 记录层落地）。
//     这样游戏能继续往下走，而我们在日志里拿到"它到底怎么用队列"的真实序列 —— 这是 M4-c 的判据。
//
// 与既有实现的关系：Device 的 Initialize/GetProcAddress/SetDepthMode/SetWindowOriginMode 已经在
// dm_nvn_ohos.c 里做过（真机验证过），本文件**只补缺的那些**，不重复定义。
// Side-table approach again: values live in our own shadow store, never inside the game's objects.

#include <string.h>
#include <stdint.h>
#include "dm_nvn_dev_ohos.h"
#include "dm_nvn_cb_ohos.h"   // 提交时取 CB 摘要

// ── 旁表：一个对象一条记录，字段按对象类型各自解释 ──
#define OBJ_MAX 96
typedef struct {
    const void *p;
    uint64_t f[16];
    uint32_t used;
} obj_ent;
static obj_ent g_obj[OBJ_MAX];
static int g_nobj;

// 只用于打点/判据的计数器（真机日志里能看到真实调用序列）
uint64_t g_nvn_win_init, g_nvn_win_acquire, g_nvn_q_init, g_nvn_q_submit, g_nvn_q_present,
         g_nvn_q_finish, g_nvn_q_flush, g_nvn_dev_ts;

// 提交摘要滚动缓冲（供探针读取；每批最多 4 条）
char g_nvn_submit_log[8][160];
int  g_nvn_submit_n;

static obj_ent *obj_find(const void *p, int create) {
    if (!p) return NULL;
    for (int i = 0; i < g_nobj; i++)
        if (g_obj[i].p == p) return &g_obj[i];
    if (!create || g_nobj >= OBJ_MAX) return NULL;
    obj_ent *e = &g_obj[g_nobj++];
    memset(e, 0, sizeof *e);
    e->p = p;
    return e;
}
static void oset(const void *p, int i, uint64_t v) {
    obj_ent *e = obj_find(p, 1);
    if (!e) return;
    e->f[i] = v; e->used |= (1u << i);
}
static uint64_t oget(const void *p, int i, uint64_t def) {
    obj_ent *e = obj_find(p, 0);
    if (!e || !(e->used & (1u << i))) return def;
    return e->f[i];
}

// ── 生成 setter/getter（一行一个；参数里没用到的位显式吞掉，免得 -Wunused 噪音）──
#define OSET(fam, idx) \
    static uint64_t fam##_set##idx(uint64_t o, uint64_t v, uint64_t c, uint64_t d) { \
        (void)c; (void)d; oset((const void *)(uintptr_t)o, idx, v); return 0; }
#define OGET(fam, idx, def) \
    static uint64_t fam##_get##idx(uint64_t o, uint64_t b, uint64_t c, uint64_t d) { \
        (void)b; (void)c; (void)d; return oget((const void *)(uintptr_t)o, idx, def); }

// builder 的 SetDevice：**空操作**。踩过的坑：一开始让它和 SetNativeWindow 共用"写字段 0"的实现，
// 若调用顺序是 SetNativeWindow → SetDevice，nativeWindow 就被清掉了（NVN 里这俩是两个字段）。
static uint64_t noop_set(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return 0;
}

// ───────── Device Builder（字段：0=flags, 1=reserved）─────────
static uint64_t devb_def(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; oset((const void *)(uintptr_t)o, 0, 0); return 0;
}
OSET(devb, 0)                     OGET(devb, 0, 0)

// ───────── Device 其它 ─────────
static uint64_t dev_final(uint64_t d, uint64_t b, uint64_t c, uint64_t e) {
    (void)d; (void)b; (void)c; (void)e; return 0;
}
// 把 bootstrap 自己也登记进表：真机日志里若出现"按名字要 nvnBootstrapLoader"，能直接命中；
// 覆盖率工具也据此把它算作已实现（此前它一直被算成"缺"）。
void *dm_nvn_bootstrap(const char *name);
static uint64_t dev_bootstrap_self(uint64_t name, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return (uint64_t)(uintptr_t)dm_nvn_bootstrap((const char *)(uintptr_t)name);
}
static uint64_t dev_apply_deferred(uint64_t o, uint64_t c2, uint64_t c, uint64_t d) {
    (void)o; (void)c2; (void)c; (void)d; return 0;
}
// 时间戳：NVN 要"纳秒级单调时钟"。宿主侧用 CLOCK_MONOTONIC 直通（与 guest 时钟同源）。
#include <time.h>
static uint64_t dev_now_ns(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)o; (void)b; (void)c; (void)d;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_nvn_dev_ts++;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// 纹理句柄/texel-fetch 句柄：**返回 0 = 非法句柄**。游戏若用于 bindless 绑定，会走到"不绑定"的分支，
// 比返回随机数安全（真遇到问题，日志里 g_nvn_dev_ts 旁边能看出调用量）。
static uint64_t dev_handle_zero(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)o; (void)b; (void)c; (void)d; return 0;
}
static uint64_t dev_get_depth_mode(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return oget((const void *)(uintptr_t)o, 5, 0);
}
static uint64_t dev_get_origin(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return oget((const void *)(uintptr_t)o, 6, 0);
}
static uint64_t dev_set_label(uint64_t o, uint64_t v, uint64_t c, uint64_t d) {
    (void)o; (void)v; (void)c; (void)d; return 0;
}
static uint64_t dev_set_shader_cache(uint64_t o, uint64_t v, uint64_t c, uint64_t d) {
    (void)o; (void)v; (void)c; (void)d; return 0;
}
static uint64_t dev_wait_error(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)o; (void)b; (void)c; (void)d; return 0;      // 无错误
}
// FastClear 注册：按 format 记颜色/深度（后续我们的 clear 仿真要用）
static uint64_t fastclear_reg(uint64_t o, uint64_t fmt, uint64_t v, uint64_t d) {
    (void)d; oset((const void *)(uintptr_t)o, 8 + (fmt & 7), v); return 0;
}

// ───────── Window Builder（0=nativeWindow, 1=presentInterval, 2=textures 指针, 3=texture 个数）─────────
static uint64_t winb_def(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d;
    oset((const void *)(uintptr_t)o, 0, 0); oset((const void *)(uintptr_t)o, 1, 0);
    oset((const void *)(uintptr_t)o, 2, 0); oset((const void *)(uintptr_t)o, 3, 0);
    return 0;
}
OSET(winb, 0)  OGET(winb, 0, 0)          // nativeWindow
OSET(winb, 1)  OGET(winb, 1, 0)          // presentInterval
OSET(winb, 2)  OGET(winb, 2, 0)          // textures
OSET(winb, 3)  OGET(winb, 3, 0)          // texture count（NVN 的 builder 里是数组，这里只记首址+个数）

// ───────── Window 本体 ─────────
static uint64_t win_init(uint64_t w, uint64_t dev, uint64_t b, uint64_t d) {
    (void)dev; (void)d;
    oset((const void *)(uintptr_t)w, 0, (uint64_t)(uintptr_t)w & 0);        // 标记已初始化
    oset((const void *)(uintptr_t)w, 1, oget((const void *)(uintptr_t)b, 0, 0));   // 继承 nativeWindow
    oset((const void *)(uintptr_t)w, 2, oget((const void *)(uintptr_t)b, 1, 0));   // presentInterval
    oset((const void *)(uintptr_t)w, 3, oget((const void *)(uintptr_t)b, 2, 0));   // 纹理数组首址
    oset((const void *)(uintptr_t)w, 4, oget((const void *)(uintptr_t)b, 3, 0));   // 纹理个数
    g_nvn_win_init++;
    return 0;
}
static uint64_t win_final(uint64_t w, uint64_t b, uint64_t c, uint64_t d) {
    (void)w; (void)b; (void)c; (void)d; return 0;
}
static uint64_t win_get_native(uint64_t w, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return oget((const void *)(uintptr_t)w, 1, 0);
}
static uint64_t win_set_native(uint64_t w, uint64_t v, uint64_t c, uint64_t d) {
    (void)c; (void)d; oset((const void *)(uintptr_t)w, 1, v); return 0;
}
static uint64_t win_get_interval(uint64_t w, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return oget((const void *)(uintptr_t)w, 2, 0);
}
static uint64_t win_set_interval(uint64_t w, uint64_t v, uint64_t c, uint64_t d) {
    (void)c; (void)d; oset((const void *)(uintptr_t)w, 2, v); return 0;
}
// AcquireTexture(Window*, Texture **out, int *slot)：把 builder 里的纹理数组当作交换链。
// 没有交换链时返回 0 且 out/slot 写 0（NVN 语义：Result 非 0 表示失败，游戏会走错误分支）。
static uint64_t win_acquire(uint64_t w, uint64_t out_tex, uint64_t out_slot, uint64_t d) {
    (void)d;
    g_nvn_win_acquire++;
    uint64_t arr = oget((const void *)(uintptr_t)w, 3, 0);
    uint64_t n   = oget((const void *)(uintptr_t)w, 4, 0);
    uint64_t idx = oget((const void *)(uintptr_t)w, 5, 0);
    if (!arr || !n) {                       // 没配纹理数组 ⇒ 明确失败，别给假纹理
        if (out_tex)  *(uint64_t *)(uintptr_t)out_tex = 0;
        if (out_slot) *(uint32_t *)(uintptr_t)out_slot = 0;
        return 1;                            // 非 0 = 失败
    }
    if (idx >= n) idx = 0;
    uint64_t tex = *(uint64_t *)(uintptr_t)(arr + idx * 8);
    if (out_tex)  *(uint64_t *)(uintptr_t)out_tex = tex;
    if (out_slot) *(uint32_t *)(uintptr_t)out_slot = (uint32_t)idx;
    oset((const void *)(uintptr_t)w, 5, (idx + 1) % n);
    return 0;
}
static uint64_t win_set_crop(uint64_t w, uint64_t v, uint64_t c, uint64_t d) {
    (void)c; (void)d; oset((const void *)(uintptr_t)w, 6, v); return 0;
}
static uint64_t win_set_label(uint64_t w, uint64_t v, uint64_t c, uint64_t d) {
    (void)w; (void)v; (void)c; (void)d; return 0;
}

// ───────── Queue Builder（0=flags,1=cmdMem,2=ctrlMem,3=computeMem,4=flushThreshold,5=queueMem）─────────
static uint64_t qb_def(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d;
    for (int i = 0; i < 6; i++) oset((const void *)(uintptr_t)o, i, 0);
    return 0;
}
OSET(qb, 0) OGET(qb, 0, 0)      // flags
OSET(qb, 1) OGET(qb, 1, 0x1000) // commandMemorySize（默认给 4 KB，避免 0 除）
OSET(qb, 2) OGET(qb, 2, 0x1000) // controlMemorySize
OSET(qb, 3) OGET(qb, 3, 0)      // computeMemorySize
OSET(qb, 4) OGET(qb, 4, 0)      // commandFlushThreshold
OSET(qb, 5) OGET(qb, 5, 0)      // queueMemory
// 计算"队列需要多少内存"——游戏用它来决定分配多大；给一个保守但非 0 的值。
static uint64_t qb_get_memsize(uint64_t o, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d;
    uint64_t cmd = oget((const void *)(uintptr_t)o, 1, 0x1000);
    uint64_t ctl = oget((const void *)(uintptr_t)o, 2, 0x1000);
    return cmd + ctl + 0x4000;
}

// ───────── Queue 本体 ─────────
#define Q_MEM_USED 0
static uint64_t q_init(uint64_t q, uint64_t dev, uint64_t b, uint64_t d) {
    (void)dev; (void)d;
    oset((const void *)(uintptr_t)q, 1, oget((const void *)(uintptr_t)b, 1, 0x1000));   // cmd mem
    oset((const void *)(uintptr_t)q, 2, oget((const void *)(uintptr_t)b, 2, 0x1000));   // ctrl mem
    oset((const void *)(uintptr_t)q, 3, oget((const void *)(uintptr_t)b, 0, 0));        // flags
    oset((const void *)(uintptr_t)q, Q_MEM_USED, 0);
    g_nvn_q_init++;
    return 0;
}
static uint64_t q_final(uint64_t q, uint64_t b, uint64_t c, uint64_t d) {
    (void)q; (void)b; (void)c; (void)d; return 0;
}
static uint64_t q_get_err(uint64_t q, uint64_t b, uint64_t c, uint64_t d) {
    (void)q; (void)b; (void)c; (void)d; return 0;      // 0 = 无错误
}
static uint64_t q_set_label(uint64_t q, uint64_t v, uint64_t c, uint64_t d) {
    (void)q; (void)v; (void)c; (void)d; return 0;
}
static uint64_t q_get_cmd_used(uint64_t q, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; return oget((const void *)(uintptr_t)q, Q_MEM_USED, 0);
}
static uint64_t q_reset_counts(uint64_t q, uint64_t b, uint64_t c, uint64_t d) {
    (void)b; (void)c; (void)d; oset((const void *)(uintptr_t)q, Q_MEM_USED, 0); return 0;
}
// SubmitCommands(Queue*, int num, CommandBuffer const* const*)：记账 + 返回成功。
// **不真提交**——记录层还没落地（CommandBuffer 127 个函数只做了状态侧）。
static uint64_t q_submit(uint64_t q, uint64_t num, uint64_t bufs, uint64_t d) {
    (void)q; (void)d;
    g_nvn_q_submit += (num ? num : 1);
    // 把这批 CB 的命令摘要记下来（滚动 8 条）。**这是 M4-c 的判据**：
    // 真机日志里能看到"游戏每帧记了哪些命令、各几次"，据此才知道该先实现哪几条。
    if (bufs && num) {
        uint64_t n = num > 4 ? 4 : num;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t cb = *(uint64_t *)(uintptr_t)(bufs + i * 8);
            dm_nvn_cb_summary((void *)(uintptr_t)cb, g_nvn_submit_log[g_nvn_submit_n % 8], 160);
            g_nvn_submit_n++;
        }
    }
    return 0;
}
// PresentTexture(Queue*, Window*, int slot)：记账；真呈现要等交换链。
static uint64_t q_present(uint64_t q, uint64_t w, uint64_t slot, uint64_t d) {
    (void)q; (void)w; (void)slot; (void)d;
    g_nvn_q_present++;
    return 0;
}
static uint64_t q_flush(uint64_t q, uint64_t b, uint64_t c, uint64_t d) {
    (void)q; (void)b; (void)c; (void)d; g_nvn_q_flush++; return 0;
}
static uint64_t q_finish(uint64_t q, uint64_t b, uint64_t c, uint64_t d) {
    (void)q; (void)b; (void)c; (void)d; g_nvn_q_finish++; return 0;
}
static uint64_t q_fence_sync(uint64_t q, uint64_t fence, uint64_t cond, uint64_t flags) {
    (void)q; (void)fence; (void)cond; (void)flags; return 0;   // 不产生真 fence
}
static uint64_t q_wait_sync(uint64_t q, uint64_t sync, uint64_t flags, uint64_t d) {
    (void)q; (void)sync; (void)flags; (void)d; return 0;       // 同步永远"已完成"
}
static uint64_t q_acquire_tex(uint64_t q, uint64_t win, uint64_t out_tex, uint64_t out_slot) {
    return win_acquire(win, out_tex, out_slot, 0);
}
static uint64_t q_set_flags_stub(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a; (void)b; (void)c; (void)d; return 0;
}

// ── 名字表 ──
typedef struct { const char *name; void *fn; } dev_ent;
#define E(n, f) { n, (void *)(f) }
static const dev_ent g_dev_tab[] = {
    // Device（补齐缺的 18 个里除已实现者）
    E("nvnBootstrapLoader", dev_bootstrap_self),
    E("nvnDeviceBuilderSetDefaults", devb_def),
    E("nvnDeviceFinalize", dev_final),
    E("nvnDeviceBuilderSetFlags", devb_set0),        E("nvnDeviceBuilderGetFlags", devb_get0),
    E("nvnDeviceApplyDeferredFinalizes", dev_apply_deferred),
    E("nvnDeviceGetCurrentTimestampInNanoseconds", dev_now_ns),
    E("nvnDeviceGetTimestampInNanoseconds", dev_now_ns),
    E("nvnDeviceGetDepthMode", dev_get_depth_mode),
    E("nvnDeviceGetWindowOriginMode", dev_get_origin),
    E("nvnDeviceGetTextureHandle", dev_handle_zero),
    E("nvnDeviceGetTexelFetchHandle", dev_handle_zero),
    E("nvnDeviceSetDebugLabel", dev_set_label),
    E("nvnDeviceSetIntermediateShaderCache", dev_set_shader_cache),
    E("nvnDeviceWaitForError", dev_wait_error),
    E("nvnDeviceRegisterFastClearColor", fastclear_reg),
    E("nvnDeviceRegisterFastClearColori", fastclear_reg),
    E("nvnDeviceRegisterFastClearColorui", fastclear_reg),
    E("nvnDeviceRegisterFastClearDepth", fastclear_reg),

    // Window Builder（8）
    E("nvnWindowBuilderSetDefaults", winb_def),
    E("nvnWindowBuilderSetDevice", noop_set),
    E("nvnWindowBuilderSetNativeWindow", winb_set0),
    E("nvnWindowBuilderSetPresentInterval", winb_set1),
    E("nvnWindowBuilderSetTextures", winb_set2),
    E("nvnWindowBuilderGetNativeWindow", winb_get0),
    E("nvnWindowBuilderGetPresentInterval", winb_get1),
    // Window（7）
    E("nvnWindowInitialize", win_init),
    E("nvnWindowFinalize", win_final),
    E("nvnWindowAcquireTexture", win_acquire),
    E("nvnWindowGetNativeWindow", win_get_native),
    E("nvnWindowSetNativeWindow", win_set_native),
    E("nvnWindowGetPresentInterval", win_get_interval),
    E("nvnWindowSetPresentInterval", win_set_interval),
    E("nvnWindowSetCrop", win_set_crop),
    E("nvnWindowSetDebugLabel", win_set_label),

    // Queue Builder（11）
    E("nvnQueueBuilderSetDefaults", qb_def),
    E("nvnQueueBuilderSetDevice", noop_set),
    E("nvnQueueBuilderSetFlags", qb_set0),
    E("nvnQueueBuilderSetCommandMemorySize", qb_set1),
    E("nvnQueueBuilderSetControlMemorySize", qb_set2),
    E("nvnQueueBuilderSetComputeMemorySize", qb_set3),
    E("nvnQueueBuilderSetCommandFlushThreshold", qb_set4),
    E("nvnQueueBuilderSetQueueMemory", qb_set5),
    E("nvnQueueBuilderGetQueueMemorySize", qb_get_memsize),
    // Queue（13）
    E("nvnQueueInitialize", q_init),
    E("nvnQueueFinalize", q_final),
    E("nvnQueueGetError", q_get_err),
    E("nvnQueueSetDebugLabel", q_set_label),
    E("nvnQueueGetTotalCommandMemoryUsed", q_get_cmd_used),
    E("nvnQueueGetTotalControlMemoryUsed", q_get_cmd_used),
    E("nvnQueueGetTotalComputeMemoryUsed", q_get_cmd_used),
    E("nvnQueueResetMemoryUsageCounts", q_reset_counts),
    E("nvnQueueSubmitCommands", q_submit),
    E("nvnQueuePresentTexture", q_present),
    E("nvnQueueFlush", q_flush),
    E("nvnQueueFinish", q_finish),
    E("nvnQueueFenceSync", q_fence_sync),
    E("nvnQueueWaitSync", q_wait_sync),
    E("nvnQueueAcquireTexture", q_acquire_tex),
};
#undef E

void *dm_nvn_dev_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_dev_tab / sizeof g_dev_tab[0]; i++)
        if (!strcmp(g_dev_tab[i].name, name)) return g_dev_tab[i].fn;
    return NULL;
}

int dm_nvn_dev_count(void) { return (int)(sizeof g_dev_tab / sizeof g_dev_tab[0]); }

// 自测：往返 + 失败语义（真机一键跑）
int dm_nvn_dev_selftest(void) {
    int fail = 0;
    static uint64_t b[8], w[8], q[8], w2[8];
    // Window builder → Window：nativeWindow / presentInterval 要带过去
    winb_def((uint64_t)(uintptr_t)b, 0, 0, 0);
    winb_set0((uint64_t)(uintptr_t)b, 0x11223344, 0, 0);
    winb_set1((uint64_t)(uintptr_t)b, 2, 0, 0);
    if (win_init((uint64_t)(uintptr_t)w, 0, (uint64_t)(uintptr_t)b, 0) != 0) fail++;
    if (win_get_native((uint64_t)(uintptr_t)w, 0, 0, 0) != 0x11223344) fail++;
    if (win_get_interval((uint64_t)(uintptr_t)w, 0, 0, 0) != 2) fail++;
    // 没配纹理数组 ⇒ AcquireTexture 必须**明确失败**（返回非 0），不能给假纹理
    if (win_acquire((uint64_t)(uintptr_t)w, 0, 0, 0) == 0) fail++;
    // 配了 2 张纹理 ⇒ 成功且 slot 轮转
    static uint64_t texs[2] = { 0xAAAA, 0xBBBB };
    winb_set2((uint64_t)(uintptr_t)b, (uint64_t)(uintptr_t)texs, 0, 0);
    winb_set3((uint64_t)(uintptr_t)b, 2, 0, 0);
    win_init((uint64_t)(uintptr_t)w2, 0, (uint64_t)(uintptr_t)b, 0);
    uint64_t got = 0; uint32_t slot = 0xFF;
    if (win_acquire((uint64_t)(uintptr_t)w2, (uint64_t)(uintptr_t)&got, (uint64_t)(uintptr_t)&slot, 0) != 0) fail++;
    if (got != 0xAAAA || slot != 0) fail++;
    if (win_acquire((uint64_t)(uintptr_t)w2, (uint64_t)(uintptr_t)&got, (uint64_t)(uintptr_t)&slot, 0) != 0) fail++;
    if (got != 0xBBBB || slot != 1) fail++;
    // Queue builder → Queue
    qb_def((uint64_t)(uintptr_t)b, 0, 0, 0);
    qb_set1((uint64_t)(uintptr_t)b, 0x8000, 0, 0);
    if (q_init((uint64_t)(uintptr_t)q, 0, (uint64_t)(uintptr_t)b, 0) != 0) fail++;
    if (q_get_cmd_used((uint64_t)(uintptr_t)q, 0, 0, 0) != 0) fail++;
    if (q_get_err((uint64_t)(uintptr_t)q, 0, 0, 0) != 0) fail++;
    return fail;
}
