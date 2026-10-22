// 通用 SDK 胶水层实现（见头文件顶部的定位说明）。
// Implementation of the general SDK glue layer (see the header for the rationale).
#include "dm_sdk_ohos.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>

static void (*g_log)(const char *, ...);
void dm_sdk_set_log(void (*fn)(const char *, ...)) { g_log = fn; }
#define SDKLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// ═══════════════ nn::vi：显示器 / 图层 / 原生窗口 ═══════════════
// 为什么必须补：这些接口返回 0 表示**成功**，但输出指针也要回写；桩不回写 ⇒ 游戏手里的
// Display/Layer/NativeWindow 恒为 0，后续取尺寸/取窗口/等垂直同步全部踩空。
// 影子对象按类型各一块（0x100），够游戏"当结构体读写"；真做 Vulkan surface 时替换这里。
static unsigned char g_vi_display[0x100];
static unsigned char g_vi_layer[0x100];
static unsigned char g_vi_native_window[0x100];
static unsigned char g_vi_vsync_event[0x40];

static int vi_init(void) { return 0; }
static int vi_open_display(uint64_t a0) {
    if (a0) *(void **)(uintptr_t)a0 = g_vi_display;
    SDKLOG("[vi] OpenDefaultDisplay → 影子 Display %p\n", (void *)g_vi_display);
    return 0;
}
static int vi_create_layer(uint64_t a0, uint64_t a1) {
    if (a0) *(void **)(uintptr_t)a0 = g_vi_layer;
    SDKLOG("[vi] CreateLayer(display=%p) → 影子 Layer %p\n", (void *)(uintptr_t)a1, (void *)g_vi_layer);
    return 0;
}
static int vi_get_native_window(uint64_t a0, uint64_t a1) {
    if (a0) *(void **)(uintptr_t)a0 = g_vi_native_window;
    SDKLOG("[vi] GetNativeWindow(layer=%p) → 影子 NativeWindow %p\n",
           (void *)(uintptr_t)a1, (void *)g_vi_native_window);
    return 0;
}
static int vi_set_scaling(uint64_t a0, uint64_t a1) { (void)a0; (void)a1; return 0; }
static int vi_get_vsync_event(uint64_t a0) {
    // 输出是游戏自己的 SystemEventType：清零＝"未触发"。若以后游戏真去等它，会阻塞 ⇒
    // 届时换成宿主假事件（每次 Wait 立即返回），与 nn::os 事件表一起做。
    if (a0) memset((void *)(uintptr_t)a0, 0, sizeof g_vi_vsync_event);
    SDKLOG("[vi] GetDisplayVsyncEvent → 零事件（暂不触发）\n");
    return 0;
}

// ═══════════════ nn::oe：运行环境（取值类给合理缺省） ═══════════════
static int oe_get_operation_mode(void) { return 0; }        // 0 = Handheld
static int oe_get_performance_mode(void) { return 0; }      // 0 = Normal
static int oe_get_desired_language(void) { return 0; }      // 0 = AmericanEnglish
static int oe_ok(uint64_t a0) { (void)a0; return 0; }       // Initialize / Set* 一律成功

// ═══════════ nn::os 事件 / 轻量事件 / 信号量（2026-10-19 np130 补，np138 换版+打点）═══════════
// 真机实证：这三族**一个都没实现**（全落通用桩、返 0）——而"等事件"是任何 Horizon 程序的骨架：
//   SignalLightEvent 被调 256 次却什么都没发生；Wait* 立刻返回"成功"，上层只能空转或判错。
//   与 libc 空桩同性质、同样与游戏无关（换游戏照用），所以放这层。
// 语义按 nnSdk：
//   Event / LightEvent：有"已置位"标志；ClearMode 0=自动复位（被取走即清）、1=手动复位。
//     Wait 阻塞到置位为止，TryWait 不阻塞，TimedWait 带超时。
//   Semaphore：计数；Acquire 为 0 时阻塞，Release 加一（也可指定个数）。

// ⚠⚠ np138 换版（2026-10-20 下午真机实证的**仪器自身缺陷**）：旧版 512 格线性表，
//   满则**所有新事件共享槽 0**——live 跑主线程创建 512+ 个事件对象（键 0x5fdf618xxx），
//   于是 init 把别人信号清零（还不加锁）、signal/clear 在槽 0 上互踩 ⇒ 信号被清/被偷
//   ⇒ 等待者永久挂死。这与 np128 互斥量表修掉的是同一类病：表满退化必须绝迹。
// 新版 = 指针键哈希 + 链式节点：固定桶数组定位、每事件一个 calloc 节点（无共享槽）；
//   节点指针终生稳定不搬迁（等待者持指针等多久都安全）；键永不删除（Finalize 是 no-op
//   语义）⇒ 不需要墓碑；分配失败走立即成功哨兵 + 红日志 ⇒ 宁可放过，不可静默挂死。
#include <pthread.h>
#include <time.h>
#include <stdlib.h>

#define DM_EV_BUCKETS 8192u        // 固定桶数（2 的幂）；实测 ~512+ 事件 ⇒ 平均链长约 0.1

typedef struct dm_ev_node {
    struct dm_ev_node *next;
    void               *key;       // 客体事件对象指针（认人的钥匙）
    int32_t             val;       // 事件：1=已置位；信号量：当前计数
    int32_t             manual;    // 1 = 手动复位
    int32_t             is_sem;
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
} dm_ev_node;

static dm_ev_node *g_ev_head[DM_EV_BUCKETS];
static pthread_mutex_t g_ev_reg = PTHREAD_MUTEX_INITIALIZER;   // 登记表自身的锁
static int g_n_ev;                                            // 已登记事件/信号量数

// 分配失败兜底：val=INT32_MAX + manual=1 ⇒ 事件 Wait 立即成功、信号量 Acquire 永不被挡
// （多个 key 在极端内存压力下会共享它，但谁都不阻塞保证不会因此造出挂死）
static dm_ev_node g_ev_dummy = { NULL, NULL, 0x7fffffff, 1, 0,
                                 PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER };

static uint32_t ev_hash(void *k) {
    uint64_t h = (uint64_t)(uintptr_t)k;
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33;   // splitmix64 收尾
    return (uint32_t)h;
}

static dm_ev_node *ev_get(void *k, int is_sem) {
    uint32_t b = ev_hash(k) & (DM_EV_BUCKETS - 1u);
    pthread_mutex_lock(&g_ev_reg);
    for (dm_ev_node *n = g_ev_head[b]; n; n = n->next)
        if (n->key == k) { pthread_mutex_unlock(&g_ev_reg); return n; }
    dm_ev_node *n = (dm_ev_node *)calloc(1, sizeof *n);
    if (!n) {
        pthread_mutex_unlock(&g_ev_reg);
        SDKLOG("【红】事件节点分配失败（%p）！走立即成功兜底（不阻塞）\n", k);
        return &g_ev_dummy;
    }
    n->key = k; n->val = 0; n->manual = 1; n->is_sem = is_sem;
    pthread_mutex_init(&n->mu, NULL);
    pthread_condattr_t ca; pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&n->cv, &ca);
    pthread_condattr_destroy(&ca);
    n->next = g_ev_head[b]; g_ev_head[b] = n;
    int ne = ++g_n_ev;
    pthread_mutex_unlock(&g_ev_reg);
    // 里程碑打点：见证新表真的在用、事件量级多少（顺带证明无共享槽路径）
    if (ne == 256 || ne == 512 || ne == 1024 || ne == 4096 || ne == 16384)
        SDKLOG("[事件] 已登记 %d 个事件/信号量（哈希表 %u 桶，链式节点，无共享槽）\n",
               ne, DM_EV_BUCKETS);
    return n;
}

// ═══ 等待族可见性（np138，2026-10-20 下午）═══
// 为什么补（真机实证的卡点）：主线程冻在 StarterPackMgr 构造器尾段而日志纯静默——
//   WaitLightEvent/TimedWaitLightEvent/AcquireSemaphore 这三个真阻塞调用旧版一行不打，
//   分不清真卡死与卡在我们的桩里。
// 打法：真正进入阻塞前打一行（key/族），离开后打一行（实测耗时）。
//   判读规则一句话：**有进入没有配对离开的行 = 卡死在这次等待上**。
// 行为中性：只加观测、不改语义 ⇒ 打点本身不可能修好一个挂死，不影响修复归因。
// 油门：进入时**一次决定**这对日志打不打（前 200 次阻塞全记，之后每 50 次记一次）——
//   必须成对决策，否则分别油门会产出"有离开无进入"的孤立行，破坏配对判读。
static int g_n_waitblk;
static int wait_log_ok(void) {
    int n = ++g_n_waitblk;
    return n <= 200 || (n % 50) == 0;
}
static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t os_init_event(uint64_t a0, uint64_t signalled, uint64_t mode) {
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 0);
    if (e != &g_ev_dummy) {
        // np138：补锁。旧版直接写 val/manual，与并发等待者构成数据竞争
        //   （等者可能撞上置位被清的中间态，白等或早退）。
        pthread_mutex_lock(&e->mu);
        e->manual = (int32_t)mode;              // EventClearMode: 0=auto, 1=manual
        e->val = (int32_t)(signalled & 1);
        pthread_mutex_unlock(&e->mu);
    }
    return 0;
}
static uint64_t os_signal_event(uint64_t a0) {
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu); e->val = 1; pthread_cond_broadcast(&e->cv); pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_clear_event(uint64_t a0) {
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu); e->val = 0; pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_try_wait_event(uint64_t a0) {
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu);
    int hit = e->val != 0;
    if (hit && !e->manual) e->val = 0;
    pthread_mutex_unlock(&e->mu);
    return (uint64_t)hit;
}
static uint64_t os_finalize_event(uint64_t a0) { (void)a0; return 0; }

// 轻量事件：同一套语义，只是另一组类型名
static uint64_t os_wait_light(uint64_t a0) {
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu);
    int log_it = 0; uint64_t t0 = 0;
    if (!e->val) {
        log_it = wait_log_ok(); t0 = now_ms();      // 成对决策：进/出用同一个 log_it
        if (log_it)
            SDKLOG("[等待] 阻塞进入 key=%#llx 族=LightEvent/Wait 值=0\n",
                   (unsigned long long)a0);
        while (!e->val) pthread_cond_wait(&e->cv, &e->mu);
    }
    if (!e->manual) e->val = 0;
    pthread_mutex_unlock(&e->mu);
    if (log_it)
        SDKLOG("[等待] 阻塞离开 key=%#llx 族=LightEvent/Wait 耗时=%llu ms\n",
               (unsigned long long)a0, (unsigned long long)(now_ms() - t0));
    return 0;
}
static uint64_t os_timedwait_light(uint64_t a0, uint64_t ns) {   // TimeSpan 按值传在 x1
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 0);
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    int64_t n = (int64_t)ns;
    if (n < 0) n = 0;
    if (n == 0) return os_try_wait_event(a0);   // 0 超时 = 不阻塞
    dl.tv_sec += n / 1000000000; dl.tv_nsec += n % 1000000000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&e->mu);
    int log_it = 0; uint64_t t0 = 0;
    if (!e->val) {
        log_it = wait_log_ok(); t0 = now_ms();
        if (log_it)
            SDKLOG("[等待] 阻塞进入 key=%#llx 族=LightEvent/TimedWait 超时=%lld ns\n",
                   (unsigned long long)a0, (long long)n);
        while (!e->val) if (pthread_cond_timedwait(&e->cv, &e->mu, &dl) != 0) break;
    }
    int hit = e->val != 0;
    if (hit && !e->manual) e->val = 0;
    pthread_mutex_unlock(&e->mu);
    if (log_it)
        SDKLOG("[等待] 阻塞离开 key=%#llx 族=LightEvent/TimedWait 命中=%d 耗时=%llu ms\n",
               (unsigned long long)a0, hit, (unsigned long long)(now_ms() - t0));
    return (uint64_t)hit;
}

// ⭐ np176：SystemEvent 家族（宿主语义 = "恒已置位、不阻塞"）——见映射表处注释。
static uint64_t os_sysevent_wait(uint64_t a0, uint64_t a1) {
    (void)a1;
    static int n;
    if (++n <= 4) SDKLOG("[os] WaitSystemEvent(%#llx) → 宿主恒已置位，立即返回\n",
                         (unsigned long long)a0);
    return 0;
}
static uint64_t os_sysevent_ok(uint64_t a0, uint64_t a1) {
    (void)a0; (void)a1;
    return 1;                        // bool：已置位
}
static uint64_t os_sysevent_void(uint64_t a0, uint64_t a1) { (void)a0; (void)a1; return 0; }

// 信号量
static uint64_t os_init_sem(uint64_t a0, uint64_t init, uint64_t maxv) {    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 1);
    (void)maxv;
    if (e != &g_ev_dummy) {
        // np138：与 os_init_event 同样补锁（计数初值也是并发可见状态）
        pthread_mutex_lock(&e->mu);
        e->val = (int32_t)init;
        pthread_mutex_unlock(&e->mu);
    }
    return 0;
}
static uint64_t os_acquire_sem(uint64_t a0) {
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 1);
    pthread_mutex_lock(&e->mu);
    int log_it = 0; uint64_t t0 = 0;
    if (e->val <= 0) {
        log_it = wait_log_ok(); t0 = now_ms();
        if (log_it)
            SDKLOG("[等待] 阻塞进入 key=%#llx 族=Semaphore/Acquire 计数=0\n",
                   (unsigned long long)a0);
        while (e->val <= 0) pthread_cond_wait(&e->cv, &e->mu);
    }
    e->val--;
    pthread_mutex_unlock(&e->mu);
    if (log_it)
        SDKLOG("[等待] 阻塞离开 key=%#llx 族=Semaphore/Acquire 耗时=%llu ms\n",
               (unsigned long long)a0, (unsigned long long)(now_ms() - t0));
    return 0;
}
static uint64_t os_release_sem(uint64_t a0, uint64_t n) {   // 单参重载时 x1 是脏值，按 ≥1 处理
    dm_ev_node *e = ev_get((void *)(uintptr_t)a0, 1);
    int32_t add = (n >= 1 && n <= 0x1000) ? (int32_t)n : 1;
    pthread_mutex_lock(&e->mu); e->val += add; pthread_cond_broadcast(&e->cv); pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_finalize_sem(uint64_t a0) { (void)a0; return 0; }

// ═══════════════ nn::settings：语言等价判定（2026-10-19 np129 补）═══════════════
// 为什么补（真机实证的卡点）：`ResourceMgrTask` 建完之后，游戏在
//   `nn::settings::eq(Language, LanguageCode)` 上打了无数个来回（PLT 日志刷满、进程 0% CPU 睡死）。
//   它拿到的一直是 0 =「不相等」⇒ 语言表轮询到底也匹配不到 ⇒ 原地死等。
// ABI：eq(const Language&, const LanguageCode&) -> bool
//   Language     = 枚举（4 字节，按 const& 传指针）
//   LanguageCode = char[8]（"en-US"/"ja" 这类，NUL 结尾）
// 表按 nnSdk 的 Language 枚举顺序（与 oe::GetDesiredLanguage 的返回值同一套）——**与游戏无关**，
// 换游戏照用；只有"我们上报哪种语言"是可配的（见 oe_get_desired_language，目前 0 = 美式英语）。
static const char *sdk_lang_code(int32_t lang) {
    switch (lang) {
    case 0:  return "en-US";
    case 1:  return "en-GB";
    case 2:  return "ja";
    case 3:  return "fr";
    case 4:  return "de";
    case 5:  return "es-419";
    case 6:  return "es";
    case 7:  return "it";
    case 8:  return "nl";
    case 9:  return "fr-CA";
    case 10: return "pt";
    case 11: return "ru";
    case 12: return "ko";
    case 13: return "zh-TW";
    case 14: return "zh-CN";
    case 15: return "pt-BR";
    default: return NULL;
    }
}
static int g_n_seteq;
static uint64_t sdk_settings_eq(uint64_t a0, uint64_t a1) {
    if (a0 < 0x10000ull || a1 < 0x10000ull) return 0;
    int32_t lang = *(const int32_t *)(uintptr_t)a0;
    const char *code = (const char *)(uintptr_t)a1;
    const char *mine = sdk_lang_code(lang);
    char cb[9]; size_t i = 0;
    for (; i < 8 && code[i]; i++) cb[i] = code[i];
    cb[i] = 0;
    int hit = (mine && !strcmp(mine, cb)) ? 1 : 0;
    if (++g_n_seteq <= 16)
        SDKLOG("[settings] eq(lang=%d「%s」, code=\"%s\") → %d\n",
               (int)lang, mine ? mine : "?", cb, hit);
    return (uint64_t)hit;
}

// ═══════════════ nn::os：时基换算与睡眠（2026-10-19 np78 补）═══════════════
// 为什么补这两个：`ConvertToTimeSpan` 此前是"一律返 0"的桩 ⇒ 游戏算出的睡眠时长恒为 0
//   ⇒ 主循环**全速空转**（真机实测 18578 帧、日志刷 3 万行，把诊断信号全淹掉）。
//   换成真换算后，循环按游戏本来的帧率跑（60 帧左右），时序逻辑也恢复。
// 换算基准与探针里的 GetSystemTick 一致：**19.2MHz 时基**（Switch 约定）
//   ⇒ 1 tick = 1000/19.2 ns = 625/12 ns（用整数乘除避免浮点）。
#include <time.h>

#define DM_TICK_HZ 19200000ull    // 19.2 MHz

// ═══ nn::util：VSNPrintf（2026-10-19 np95 补）═══
// **这是"假消息串"的根因**：游戏第一步就调它格式化日志/字符串，而它此前是"一律返 0"的桩
//   ⇒ 缓冲区一个字都没写 ⇒ 游戏手里所有字符串都是垃圾（解析的键、日志消息全是野指针）。
// ABI：VSNPrintf(char *buf, size_t size, const char *fmt, va_list ap) —— ap 是指针，
//   与宿主 vsnprintf 的 va_list 同为 AAPCS 布局，可直接转发。
#include <stdarg.h>
#include <stdio.h>
static int g_n_vsn;
static uint64_t util_vsnprintf(uint64_t buf, uint64_t size, uint64_t fmt, uint64_t ap) {
    if (!buf || !fmt || !ap) return 0;
    if (ap == 0x5f000000000000ull || (ap >> 48) == 0x5f) {
        // np96：分派器此前只转发 3 个参数，x3（va_list 指针）被丢 ⇒ 这里读到的是垃圾
        //   （实测 va=0x5f000000000000）。现在分派器已转发 x3，这个分支仅作防御。
        SDKLOG("[util] VSNPrintf 收到可疑 va=%#llx，跳过\n", (unsigned long long)ap);
        return 0;
    }
    if (++g_n_vsn <= 6) {
        char fb[40]; size_t i = 0;
        const char *f = (const char *)(uintptr_t)fmt;
        if (fmt > 0x10000ull && fmt < 0x800000000000ull)
            for (; i < sizeof fb - 1 && f[i]; i++) fb[i] = f[i];
        fb[i] = 0;
        SDKLOG("[util] VSNPrintf#%d fmt=\"%s\" va=%#llx\n", g_n_vsn, fb,
               (unsigned long long)ap);
    }
    // np97：分派器已正确转发 x3（va_list 指针在客体堆，np96 实测 va=0x5b98… 合法），
    //   可以真格式化了。之前"假消息串"（解析键/日志消息全是垃圾）的根因正是这里。
    va_list apc;
    __builtin_va_copy(apc, *(va_list *)(uintptr_t)ap);
    int n = vsnprintf((char *)(uintptr_t)buf, (size_t)size,
                      (const char *)(uintptr_t)fmt, apc);
    __builtin_va_end(apc);
    return (uint64_t)(n > 0 ? n : 0);
}

static uint64_t os_convert_timespan(uint64_t tick) {
    // nn::os::ConvertToTimeSpan(Tick) -> TimeSpan（{int64 ns}，按值返回 ⇒ x0）
    return (uint64_t)tick * 625u / 12u;
}
static uint64_t os_sleep(uint64_t ns) {
    // ⭐ np174（2026-10-21）：`SleepThread(TimeSpan(0))` 在 NX 上的语义是**让出剩余时间片**
    //   （yield），不是空操作。游戏的空闲/等待环正是 `ConvertToTimeSpan(0) → SleepThread(0)`；
    //   返回 0 让它退化成**纯自旋**——np173 真机实证：轨迹丢弃 27,185,391 条、线程快照里
    //   多个线程 state=R、主线程也是 R 却不再调任何导入（CPU 被空转抢光）。
    //   ⇒ ns==0 走 sched_yield()。这是"宿主语义对齐"，换任何游戏都成立。
    if (!ns) { sched_yield(); return 0; }
    {   // np79：看游戏到底要睡多久（前 20 次 + 任何 >100ms 的都记）
        static int n;
        if (n < 20 || ns > 100000000ull) {
            if (n < 20) n++;
            SDKLOG("[os] SleepThread(%llu ns)%s\n", (unsigned long long)ns,
                   ns > 100000000ull ? " ← 超过 100ms" : "");
        }
    }
    if (ns > 1000000000ull * 30ull) {
        // 离谱的时长（>30s）多半是上游算错：照睡会把测试挂死，这里截断并记录。
        static int n;
        if (++n <= 5) SDKLOG("[os] SleepThread 时长离谱 %llu ns，截断为 100ms\n",
                             (unsigned long long)ns);
        ns = 100000000ull;
    }
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
    return 0;
}

// ═══════════════ C++ 静态局部对象：初始化守卫（2026-10-20 np145 补）═══════════════
//
// 背景（为什么要专门实现，缺了会怎样）：
//   游戏里有 **25179 个** `bl __cxa_guard_acquire` 站点、18197 个是"判据型"——
//   编译器为每个"函数内静态局部对象"生成这样一段：
//       ldarb w8, [guard]        ; 读 guard 字节
//       tbnz  w8, #0, skip       ; 判据：**bit0 置位 ⇒ 已完成**
//       bl    __cxa_guard_acquire(guard)
//       cbz   w0, skip           ; 返回 0 ⇒ 别人已初始化 ⇒ 跳过
//       <初始化代码>
//       bl    __cxa_guard_release(guard)
//   宿主此前把 acquire 实现成"恒返回 1"、release 实现成"空操作"，
//   **且从不写 guard 字节** ⇒ 静态局部初始化器**每次被调用都重跑一遍**
//   （重复写字段、重复加解锁、重复分配）。
//
// 协议（Itanium C++ ABI）：acquire 返回 1 = "该你初始化"；release 之后 guard 标记为完成。
//   我们用**字节状态机**实现：
//     FREE(0) ──acquire CAS 成功──> BUSY(4) ──release──> DONE(2)
//   ★ 为什么 BUSY 用 4 而不是标准的 1：
//     **游戏自己的判据测的是 bit0**（`tbnz w8,#0`）。若 BUSY=1（bit0 置位），
//     别的线程在初始化期间走这条判据会**误以为"已完成"而跳过初始化**，
//     拿到半成品对象。BUSY=4（bit0 清零）⇒ 游戏判据永不误判，
//     而是照常去调 acquire，由我们在这里等待并返回 0 ⇒ 两条路都正确。
//     DONE=2 是 ABI 标准值（bit0=0）⇒ 完成后游戏仍会调 acquire，我们返回 0 → 跳过初始化。
//   ⇒ 结论：**判据永远由 acquire 的返回值裁决**，字节值只影响性能与并发安全。
//
// 归属：本语义**与游戏无关**（任何 C++ 程序都这么用）⇒ 属通用框架能力，故住本层。

// ── 可写性闸门：写 guest 内存前先确认那一页真的可写 ──
// 为什么需要：guard 字节分布在映像各处（25179 个站点），而映像的
//   [0, text_end) 与 [patch_off, end) 被封成 R+X（见 map_image_ohos.c 的 seal），
//   其余区段才可写。若某个 guard 恰好落在 R+X 段，直接写 ⇒ 当场 SIGSEGV，
//   而这个崩会被误当成"游戏的问题"。所以：**先查，再写**。
//
// np146 实测教训：第一版只看 /proc/self/maps，结果**27 个 guard 地址全被判不可写**
//   ⇒ 正确性与性能双输。根因两条：
//    ① maps 缓冲区只给了 16KB，而本进程装载 46MB 映像 + sdk，maps 行数远超
//       16KB ⇒ 目标区域被截断 ⇒ "找不到 ⇒ 不敢写"（ conservative 但全错）；
//    ② 更要紧的：**真正要防的 R+X 封区边界是已知常量**（text_end / patch_off /
//       img_end，见下），不必去翻进程全表。
//   所以本版改成**已知封区优先 + maps 兜底**。
//
// 通用能力：与游戏无关。封区边界由探针在装载时注入（`dm_sdk_set_sealed_ranges`），
//   未注入时退化为"只认 maps"，再查不到时放行（宿主堆分配几乎必然可写）。

// 由探针注入的"主映像封区"（R+X 区间）。基址是运行期才知的，故按 [base+off, base+off) 存。
typedef struct { uint64_t base, lo, hi; } dm_seal_t;
static dm_seal_t g_seal[4];
static int g_seal_n;

// 注入主映像的 R+X 封区。lo/hi 是**映像内偏移**，base 是运行期基址。
void dm_sdk_set_sealed_ranges(uint64_t base, const uint64_t (*ranges)[2], int n) {
    g_seal_n = 0;
    if (!base || !ranges || n <= 0) return;
    for (int i = 0; i < n && i < 4; i++) {
        if (ranges[i][0] >= ranges[i][1]) continue;
        g_seal[g_seal_n].base = base;
        g_seal[g_seal_n].lo   = ranges[i][0];
        g_seal[g_seal_n].hi   = ranges[i][1];
        g_seal_n++;
    }
}

static int dm_addr_in_sealed(uint64_t addr) {
    for (int i = 0; i < g_seal_n; i++)
        if (addr >= g_seal[i].base + g_seal[i].lo &&
            addr <  g_seal[i].base + g_seal[i].hi)
            return 1;
    return 0;
}

// ── /proc/self/maps 兜底 ──
// 缓冲区给 256KB（本进程 maps 实测远大于 16KB；宁可多占静态区也别再截断）。
// ⚠ fopen 失败时不把 maps_loaded 置 1，下次重试（第一版在这儿把失败当成功缓存）。
#define DM_MAPS_BUF (256 * 1024)
static char g_maps[DM_MAPS_BUF];
static size_t g_maps_len;
static int g_maps_ok;

static void dm_maps_load(void) {
    if (g_maps_ok) return;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    g_maps_len = fread(g_maps, 1, sizeof g_maps - 1, f);
    fclose(f);
    if (g_maps_len == 0) return;          // 读失败：不当成功
    g_maps[g_maps_len] = 0;
    g_maps_ok = 1;
}

// 返回 1=可写 / 0=不可写或不明
static int dm_maps_writable(uint64_t addr) {
    dm_maps_load();
    if (!g_maps_ok) return 0;
    const char *p = g_maps;
    while (p && *p) {
        unsigned long long lo = 0, hi = 0;
        char perms[8] = {0};
        int n = 0;
        // 格式：start-end perms offset dev inode path
        if (sscanf(p, "%llx-%llx %7s%n", &lo, &hi, perms, &n) == 3 && n > 0) {
            if (addr >= lo && addr < hi) return (perms[1] == 'w');
            const char *nl = strchr(p, '\n');
            if (!nl) break;
            p = nl + 1;
        } else {
            const char *nl = strchr(p, '\n');
            if (!nl) break;
            p = nl + 1;
        }
    }
    return -1;      // 不在缓存的这段表里（可能被截断）⇒ 明确区分"查无此页"
}

// ── 页级缓存 ──
// ⚠ 性能：guard 调用**极其频繁**（25175 个站点，每次进入都可能调 acquire），
//   若每次都线性扫 256KB 的 maps 文本 ⇒ 启动期热点。以 4KB 页为单位记住结果。
//   直接映射表（页号低 16 位做索引）——O(1)。
//   ⚠ 只缓存"不可写"的**否定结论**（2）。肯定结论（1）不必缓存：
//     acquire 对同一 guard 的成功 CAS 只会发生一次，之后字节=2 直接短路，不再过闸门；
//     而缓存肯定结论反有风险（同一页内权限可能不同）。
#define DM_WR_CACHE_BITS 16
#define DM_WR_CACHE_SIZE (1u << DM_WR_CACHE_BITS)
static uint8_t g_wr_cache[DM_WR_CACHE_SIZE];         // 0=未知 2=不可写

static inline int dm_addr_writable(uint64_t addr) {
    if (addr < 0x1000ull || addr >= 0x800000000000ull) return 0;
    uint32_t idx = (uint32_t)((addr >> 12) & (DM_WR_CACHE_SIZE - 1));   // 页粒度键
    if (g_wr_cache[idx] == 2) return 0;

    // ① 封区优先：命中已知 R+X  ⇒ 不可写
    if (dm_addr_in_sealed(addr)) { g_wr_cache[idx] = 2; return 0; }
    // ② maps 兜底：明确查到"不可写" ⇒ 不可写
    int m = dm_maps_writable(addr);
    if (m == 0) { g_wr_cache[idx] = 2; return 0; }
    // ③ 都查不到（未注入封区、maps 也没覆盖到）：**放行**。
    //    依据：宿主自己的堆/匿名映射几乎必然可写；真正危险的 R+X 段已由①拦住。
    //    宁可漏拦，也不能把正确的写入全拦掉（np146 就是全拦掉的教训）。
    return 1;
}

#define DM_GUARD_FREE 0
#define DM_GUARD_BUSY 4
#define DM_GUARD_DONE 2

#include <sched.h>
#include <stdio.h>
#include <time.h>

static uint64_t g_guard_acq_n;      // acquire 总次数
static uint64_t g_guard_init_n;     // acquire 返回 1（真要初始化）的次数
static uint64_t g_guard_dup_n;      // 同一 guard 地址第二次拿到初始化权（= 初始化器重跑，实锤）
static uint64_t g_guard_busy_n;     // 撞上别人正在初始化（等待次数）
static uint64_t g_guard_reentry_n;  // 同线程重入（递归初始化）
static uint64_t g_guard_abort_n;    // abort 次数
static uint64_t g_guard_ro_n;        // 落在不可写页、被闸门拦下的次数
static uint64_t g_guard_addr_n;     // 不同 guard 地址数（哈希表条目数）

// ── "拿到过初始化权"的地址哈希表（开放寻址）──
// 用途：**实证初始化器是否只跑一次**。语义上 acquire 对同一地址至多返回 1 一次
//   ⇒ 第二次出现即为异常（初始化器重跑）。
// 为什么用哈希表而不是线性表：站点 25179 个，每次首次都线性扫 ⇒ O(n²) 会拖慢启动。
#define DM_GUARD_HASH_BITS 16
#define DM_GUARD_HASH (1u << DM_GUARD_HASH_BITS)
static uint64_t g_guard_hash[DM_GUARD_HASH];

static inline uint32_t guard_hash_of(uint64_t g) {
    uint64_t h = (g >> 3) * 0x9E3779B97F4A7C15ull;
    return (uint32_t)((h >> 40) & (DM_GUARD_HASH - 1));
}

// 返回 1 = 首次见到该地址；0 = 见过（⇒ 初始化器重跑）
static int guard_mark_new(uint64_t g) {
    uint32_t i = guard_hash_of(g);
    for (uint32_t k = 0; k < DM_GUARD_HASH; k++) {
        uint32_t j = (i + k) & (DM_GUARD_HASH - 1);
        if (g_guard_hash[j] == 0) { g_guard_hash[j] = g; g_guard_addr_n++; return 1; }
        if (g_guard_hash[j] == g) return 0;
    }
    return 0;    // 表满（不该发生：25179 < 65536）
}

// ── 同线程重入保护 ──
// 静态局部初始化器若递归触发自身（guard 再被 acquire），会撞上我们写的 BUSY 值。
//   此时若傻等 ⇒ 死锁；若返回 1 ⇒ 内层又执行一遍初始化体 ⇒ 无限递归。
//   ⇒ 返回 0（跳过初始化、直接用半成品）最安全：不死锁也不递归。
//   （C++ 递归初始化本就是 UB，游戏自己不会这么写；这里只是不让它挂死。）
#define DM_GUARD_RECURSE_MAX 64
static __thread uint64_t g_guard_held[DM_GUARD_RECURSE_MAX];
static __thread int g_guard_held_n;

static int guard_held_by_me(uint64_t g) {
    for (int i = 0; i < g_guard_held_n; i++)
        if (g_guard_held[i] == g) return 1;
    return 0;
}
static void guard_hold_me(uint64_t g) {
    if (g_guard_held_n < DM_GUARD_RECURSE_MAX) g_guard_held[g_guard_held_n++] = g;
}
static void guard_unhold_me(uint64_t g) {
    for (int i = 0; i < g_guard_held_n; i++)
        if (g_guard_held[i] == g) {
            g_guard_held[i] = g_guard_held[--g_guard_held_n];
            return;
        }
}

static int cxa_guard_acquire(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1; (void)a2; (void)a3;
    g_guard_acq_n++;
    uint64_t g = a0;
    if (g < 0x1000ull || g >= 0x800000000000ull) {
        SDKLOG("【红】__cxa_guard_acquire 收到可疑 guard 地址 %#llx（不写内存）\n",
               (unsigned long long)g);
        return 0;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)g;

    // 同线程重入：直接返回 0（不死锁、不递归）。
    if (guard_held_by_me(g)) {
        g_guard_reentry_n++;
        if (g_guard_reentry_n <= 8)
            SDKLOG("[guard] 同线程重入 guard=%#llx（累计 %llu）⇒ 跳过初始化，不死锁\n",
                   (unsigned long long)g, (unsigned long long)g_guard_reentry_n);
        return 0;
    }

    // 可写性闸门：不可写就**不碰内存**，按"已完成"处理（跳过初始化），
    //   宁可漏初始化也不能因此崩掉整个进程。
    //   ⚠ np146 教训：闸门判据必须分清"真在 R+X 封区"与"maps 查不到"——
    //      后者已在 dm_addr_writable 里改为**放行**（宿主堆几乎必然可写）。
    if (!dm_addr_writable(g)) {
        g_guard_ro_n++;
        if (g_guard_ro_n <= 8)
            SDKLOG("【黄】guard %#llx 在**已知封区/不可写页**内 ⇒ 不写字节（累计 %llu）\n",
                   (unsigned long long)g, (unsigned long long)g_guard_ro_n);
        return 0;
    }

    uint8_t c = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    if (c == DM_GUARD_DONE) return 0;            // 已完成 ⇒ 不是我初始化

    uint8_t expected = DM_GUARD_FREE;
    if (__atomic_compare_exchange_n(p, &expected, (uint8_t)DM_GUARD_BUSY, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        g_guard_init_n++;
        guard_hold_me(g);
        int first = guard_mark_new(g);
        if (!first) {
            g_guard_dup_n++;
            SDKLOG("【红】同一 guard %#llx **第二次**拿到初始化权 ⇒ 初始化器在重跑！"
                   "（acquire 总 %llu / 初始化 %llu / 重跑 %llu）\n",
                   (unsigned long long)g, (unsigned long long)g_guard_acq_n,
                   (unsigned long long)g_guard_init_n, (unsigned long long)g_guard_dup_n);
        } else if (g_guard_init_n <= 20) {
            SDKLOG("[guard] acquire#%llu guard=%#llx ⇒ 我来初始化（不同 guard %llu 个）\n",
                   (unsigned long long)g_guard_acq_n, (unsigned long long)g,
                   (unsigned long long)g_guard_addr_n);
        }
        return 1;                                  // 由我初始化
    }

    // 撞上别人正在初始化（expected 被改成别的值）⇒ 等它完成，然后返回 0。
    // ⚠ 上限取 200ms 而不是 2 秒：站点 25179 个，若游戏真并发且对方卡住，
    //   2 秒 × 大量站点 = 主循环被拖死；200ms 足以覆盖一次正常初始化，
    //   仍超时则按"别人已完成"处理（宁可跳过初始化，也别挂死）。
    g_guard_busy_n++;
    for (int spin = 0; spin < 200; spin++) {       // 上限 ~200ms
        struct timespec ts = { 0, 1000000 };       // 1ms
        nanosleep(&ts, NULL);
        if (__atomic_load_n(p, __ATOMIC_ACQUIRE) != DM_GUARD_BUSY) {
            if (g_guard_busy_n <= 8)
                SDKLOG("[guard] 等别人初始化完（guard=%#llx，累计 %llu 次）\n",
                       (unsigned long long)g, (unsigned long long)g_guard_busy_n);
            return 0;
        }
    }
    SDKLOG("【红】guard %#llx 的初始化等超时（%llu 次）⇒ 按'别人已完成'处理\n",
           (unsigned long long)g, (unsigned long long)g_guard_busy_n);
    return 0;
}

static int cxa_guard_release(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1; (void)a2; (void)a3;
    uint64_t g = a0;
    if (g < 0x1000ull || g >= 0x800000000000ull) return 0;
    guard_unhold_me(g);
    uint8_t *p = (uint8_t *)(uintptr_t)g;
    // 只有"确实处于 BUSY（我持有）"才写 DONE。
    //   幂等：已是 DONE 则不动（重复 release 无害）。
    //   若是 FREE（不该发生 ⇒ release 被无 acquire 地调用）：**不写**，
    //   保持 FREE 让初始化器还能重试——否则这个静态局部对象会被永久标记成
    //   "已完成"而永远不初始化（比重复初始化更糟）。
    if (dm_addr_writable(g)) {
        uint8_t cur = __atomic_load_n(p, __ATOMIC_ACQUIRE);
        if (cur == DM_GUARD_BUSY || cur == DM_GUARD_DONE)
            __atomic_store_n(p, (uint8_t)DM_GUARD_DONE, __ATOMIC_RELEASE);
        else
            SDKLOG("【黄】guard %#llx 在非 BUSY 状态(%u)下收到 release ⇒ 不写 DONE，"
                   "保留重试机会\n", (unsigned long long)g, (unsigned)cur);
    }
    return 0;
}

static int cxa_guard_abort(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    (void)a1; (void)a2; (void)a3;
    uint64_t g = a0;
    g_guard_abort_n++;
    if (g < 0x1000ull || g >= 0x800000000000ull) return 0;
    guard_unhold_me(g);
    uint8_t *p = (uint8_t *)(uintptr_t)g;
    if (dm_addr_writable(g))
        __atomic_store_n(p, (uint8_t)DM_GUARD_FREE, __ATOMIC_RELEASE);   // 允许重来
    return 0;
}

// 统计摘要（探针在帧线程 tick 上调用；挂宿主线程永不调度 ⇒ 只能借游戏自己的线程）
void dm_sdk_guard_report(void) {
    SDKLOG("[guard] 统计：acquire %llu 次 / 我初始化 %llu 次 / 不同 guard %llu 个 / "
           "重跑 %llu / 等别人 %llu / 重入 %llu / abort %llu / 不可写 %llu\n",
           (unsigned long long)g_guard_acq_n, (unsigned long long)g_guard_init_n,
           (unsigned long long)g_guard_addr_n, (unsigned long long)g_guard_dup_n,
           (unsigned long long)g_guard_busy_n, (unsigned long long)g_guard_reentry_n,
           (unsigned long long)g_guard_abort_n, (unsigned long long)g_guard_ro_n);
}

// ═══════════════ 分派表 ═══════════════
// 约定：全部按"最多 3 个入参 + 32 位返回值"调用（足够覆盖这批轻量接口）。
// 需要更多入参的（如 CreateLayer 的第二参）已在调用点取出；返回 64 位的接口另开分支。
typedef struct {
    const char *sym;
    int (*fn)(uint64_t, uint64_t, uint64_t, uint64_t);
} dm_sdk_ent;

// ───────── M2（2026-10-22）：启动路径 5 个服务的最小实现 ─────────
// 为什么必须由宿主给：main 的 PLT 槽全指向**记录型存根** → `dm_sdk_call(名字,…)`，
// 表里没有的符号就**直接返回 0**（不是"崩"，是"成功但什么都没做"）。hid 那 36 次/帧的
// GetNpadStyleSet 就是这么"静默成功"的。所以服务必须加进这张表，而不是去实现 CMIF 协议。
// 语义取"最保守的真值"：**没有手柄**（GetNpadStyleSet=0）、其余初始化成功、用户给一个假句柄。
// 这样游戏看到的世界是一致的，不会拿到未初始化的栈当返回值。
static int svc_hid_init_npad(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)b;(void)c;(void)d; (void)a; return 0;               // void → 成功
}
static int svc_hid_init_debugpad(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a;(void)b;(void)c;(void)d; return 0;
}
static int svc_hid_set_npad(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a;(void)b;(void)c;(void)d; return 0;                // 记录诉求即可，无手柄可配
}
static int svc_hid_get_style(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    // GetNpadStyleSet(const NpadIdType&) → u32。返回 0 = 该 pad 没接（每帧 36 次的调用）
    (void)a;(void)b;(void)c;(void)d; return 0;
}
static int svc_account_init(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a;(void)b;(void)c;(void)d; return 0;
}
static int svc_account_open_user(uint64_t out, uint64_t b, uint64_t c, uint64_t d) {
    (void)b;(void)c;(void)d;
    // **必须写出参数**：调用方会拿这个句柄去查用户名/图标；不写就是未初始化栈
    if (out) *(uint64_t *)(uintptr_t)out = 0x1000000000000001ull;
    return 0;
}
// GetDebugPadState(DebugPadState *out)：**必须把结构体写满**，否则调用方读未初始化栈。
// 大小是离线量出来的：detail::GetDebugPadState 里对结构体指针的最大写是 `str xzr,[x20,#0x38]`
// （8 字节）⇒ 结构体 ≥ 0x40，且 NN 的结构体 8 字节对齐 ⇒ 按 0x40 清零。
// Size measured offline: the deepest store is `str xzr,[x20,#0x38]` (8 bytes) ⇒ struct is 0x40.
#define DEBUG_PAD_STATE_BYTES 0x40
static int svc_hid_debug_pad_state(uint64_t out, uint64_t b, uint64_t c, uint64_t d) {
    (void)b;(void)c;(void)d;
    if (out) memset((void *)(uintptr_t)out, 0, DEBUG_PAD_STATE_BYTES);   // 全零 = 没有按键/摇杆居中
    return 0;
}
static int svc_ret_ok(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    (void)a;(void)b;(void)c;(void)d; return 0;                // pctl / aoc 的 Initialize
}

#define E(name, f) { name, (int (*)(uint64_t, uint64_t, uint64_t, uint64_t))(f) }

static const dm_sdk_ent g_tab[] = {
    // ---- M2：启动路径实测用到的 5 个服务（hid / account / pctl / aoc）----
    // 依据：真机日志里 [PLT] 记名统计——hid 每帧 36 次 GetNpadStyleSet、account/pctl/aoc 各一次 Initialize
    E("_ZN2nn3hid14InitializeNpadEv",        svc_hid_init_npad),
    E("_ZN2nn3hid18InitializeDebugPadEv",    svc_hid_init_debugpad),
    E("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", svc_hid_set_npad),
    E("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", svc_hid_set_npad),
    E("_ZN2nn3hid15GetNpadStyleSetERKj",     svc_hid_get_style),
    E("_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE", svc_hid_debug_pad_state),
    E("_ZN2nn7account10InitializeEv",        svc_account_init),
    E("_ZN2nn7account19OpenPreselectedUserEPNS0_10UserHandleE", svc_account_open_user),
    E("_ZN2nn4pctl10InitializeEv",           svc_ret_ok),
    E("_ZN2nn3aoc10InitializeEv",            svc_ret_ok),
    // ---- nn::vi ----
    E("_ZN2nn2vi10InitializeEv", vi_init),
    E("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_display),
    E("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer),
    E("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window),
    E("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_set_scaling),
    E("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_vsync_event),
    // ---- nn::oe ----
    E("_ZN2nn2oe10InitializeEv", oe_ok),
    E("_ZN2nn2oe16GetOperationModeEv", oe_get_operation_mode),
    E("_ZN2nn2oe18GetPerformanceModeEv", oe_get_performance_mode),
    E("_ZN2nn2oe18GetDesiredLanguageEv", oe_get_desired_language),
    E("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_ok),
    E("_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_ok),
    E("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_ok),
    E("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_ok),
    E("_ZN2nn2oe17FinishStartupLogoEv", oe_ok),
    // ---- nn::util ----
    E("_ZN2nn4util9VSNPrintfEPcmPKcSt9__va_list", util_vsnprintf),
    // ---- nn::settings ----
    E("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", sdk_settings_eq),
    // ---- nn::os：事件 / 轻量事件 / 信号量（np130）----
    E("_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE", os_init_event),
    E("_ZN2nn2os11SignalEventEPNS0_9EventTypeE", os_signal_event),
    E("_ZN2nn2os10ClearEventEPNS0_9EventTypeE", os_clear_event),
    E("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE", os_try_wait_event),
    E("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE", os_finalize_event),
    E("_ZN2nn2os20InitializeLightEventEPNS0_14LightEventTypeEbNS0_14EventClearModeE", os_init_event),
    E("_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", os_signal_event),
    E("_ZN2nn2os15ClearLightEventEPNS0_14LightEventTypeE", os_clear_event),
    E("_ZN2nn2os14WaitLightEventEPNS0_14LightEventTypeE", os_wait_light),
    E("_ZN2nn2os19TimedWaitLightEventEPNS0_14LightEventTypeENS_8TimeSpanE", os_timedwait_light),
    E("_ZN2nn2os18FinalizeLightEventEPNS0_14LightEventTypeE", os_finalize_event),
    E("_ZN2nn2os19InitializeSemaphoreEPNS0_13SemaphoreTypeEii", os_init_sem),
    E("_ZN2nn2os16AcquireSemaphoreEPNS0_13SemaphoreTypeE", os_acquire_sem),
    E("_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeE", os_release_sem),
    E("_ZN2nn2os16ReleaseSemaphoreEPNS0_13SemaphoreTypeEi", os_release_sem),
    E("_ZN2nn2os17FinalizeSemaphoreEPNS0_13SemaphoreTypeE", os_finalize_sem),
    // ⭐ np176：**SystemEvent 家族按"恒已置位/不阻塞"语义**（宿主没有系统级事件源）。
    //   依据：主库只导入 Wait/TryWait/TimedWait/DestroySystemEvent，**没有** Initialize/Signal
    //   ⇒ 这些对象是 sdk 模块自己造的，我们看不到它的信号；真机 dump 显示对象 0x40 字节全 0
    //   （未置位），而唯一调用点会对同一对象连等两次 ⇒ 回"没信号"只会让上层反复重试/自旋。
    //   宿主侧能自洽提供的语义只有"不阻塞"：Wait 立即返回（原本如此），Try/TimedWait 回"已置位"。
    E("_ZN2nn2os15WaitSystemEventEPNS0_15SystemEventTypeE", os_sysevent_wait),
    E("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE", os_sysevent_ok),
    E("_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE", os_sysevent_ok),
    E("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE", os_sysevent_void),
    // ---- nn::os：时基与睡眠 ----
    E("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE", os_convert_timespan),
    E("_ZN2nn2os11SleepThreadENS_8TimeSpanE", os_sleep),
    // ---- C++ ABI：静态局部对象的初始化守卫（np145）----
    // 25179 个站点；缺了它 ⇒ 静态局部初始化器每次调用都重跑（详见本文件 guard 节注释）。
    E("__cxa_guard_acquire", cxa_guard_acquire),
    E("__cxa_guard_release", cxa_guard_release),
    E("__cxa_guard_abort", cxa_guard_abort),
};

int dm_sdk_call(const char *sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t *ret) {
    if (!sym) return 0;
    for (size_t i = 0; i < sizeof g_tab / sizeof g_tab[0]; i++)
        if (!strcmp(g_tab[i].sym, sym)) {
            int r = g_tab[i].fn(a0, a1, a2, a3);
            if (ret) *ret = (uint64_t)(uint32_t)r;   // Result / 枚举都是 32 位
            return 1;
        }
    return 0;
}
