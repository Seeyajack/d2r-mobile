// 通用 NVN 胶水层实现（见头文件顶部的定位说明）。
// Implementation of the general NVN glue layer (see the header).
#include "dm_nvn_ohos.h"
#include "dm_nvn_state_ohos.h"   // M4-a：状态族（纯数据）
#include "dm_nvn_dev_ohos.h"     // M4-c：设备/窗口/队列
#include "dm_nvn_cb_ohos.h"      // M4-c：命令缓冲记录层

#include <stddef.h>
#include <string.h>

static void (*g_log)(const char *, ...);
void dm_nvn_set_log(void (*fn)(const char *, ...)) { g_log = fn; }
#define NLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// ── 设备对照物的状态（原点/深度模式只是"记下来"，真正生效在 Vulkan 后端做投影换算）──
typedef struct {
    uint32_t magic;      // 'NVND'
    int32_t  origin;     // NVNwindowOriginMode：0=左下(GL) 1=左上(D3D/Metal)
    int32_t  depth_mode; // NVNdepthMode：0=近=-w(GL) 1=近=0(D3D/Metal)
} dm_nvn_dev;

// ── 设备属性表：**Tegra X1 实测值**，直接搬自上游 dm/gfx/nvn/hle_nvn.c 的 g_devinfo ──
// 来源：nnheaders nvn_DeviceConstantsNX.h；上游在 SMO 上逐项调 nvnDeviceGetInteger 得到。
// 为什么不能返 0：这些值是**上限/对齐/粒度**，返 0 会让游戏算出 0 尺寸缓冲、0 对齐等。
static const int g_devinfo[] = {
    // ⚠ 2026-10-19 真机实证（旷野之息）：Volante 图形初始化在 0x9fc658 做 `cmp w8,#0x34`（必须**恰好 52**），
    //   不等就整段早退 ⇒ 显示对象（+0x128）永远不建 ⇒ 帧循环读空指针崩在 0xa041e0。
    //   上游 D2R/SMO 测出来的是 53；这两个游戏要求的 API 版本不同 ⇒ **这一格是"按游戏"的**，
    //   换游戏若报同样的早退，先看这里（真机判据：0x9fc658 的立即数）。
    [0x00] = 52, [0x01] = 311,   // NVN API 版本（BotW 要求 52）、厂商/设备码
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};

static int g_n_unknown;

static uint64_t nvn_dev_get_integer(uint64_t dev, uint64_t what, uint64_t out) {
    (void)dev;
    unsigned w = (unsigned)what;
    int v;
    if (w < sizeof g_devinfo / sizeof *g_devinfo) {
        v = g_devinfo[w];
    } else {
        v = 0;
        if (++g_n_unknown <= 20)
            NLOG("【红】nvnDeviceGetInteger 未知项 %#x，给 0（补表见 dm_nvn_ohos.c）\n", w);
    }
    if (out) *(int *)(uintptr_t)out = v;
    return 0;   // void
}

static uint64_t nvn_dev_set_origin(uint64_t dev, uint64_t m) {
    if (dev) ((dm_nvn_dev *)(uintptr_t)dev)->origin = (int32_t)m;
    NLOG("[nvn] 窗口原点 %s（记下，投影换算在 Vulkan 后端做）\n", m ? "左上" : "左下");
    return 0;
}
static uint64_t nvn_dev_set_depth(uint64_t dev, uint64_t m) {
    if (dev) ((dm_nvn_dev *)(uintptr_t)dev)->depth_mode = (int32_t)m;
    NLOG("[nvn] 深度模式 %s\n", m ? "近=0" : "近=-w");
    return 0;
}
// 句柄换算：分离贴图/采样器句柄的值 = 描述符索引（采样器索引放 20 位以上，与组合句柄位布局一致）
static uint64_t nvn_dev_get_image_handle(uint64_t dev, uint64_t id) { (void)dev; return id; }
static uint64_t nvn_dev_get_tex_handle(uint64_t dev, uint64_t id) { (void)dev; return id; }
static uint64_t nvn_dev_get_samp_handle(uint64_t dev, uint64_t id) { (void)dev; return id << 20; }
static uint64_t nvn_dev_noop(uint64_t a, uint64_t b, uint64_t c) { (void)a; (void)b; (void)c; return 0; }

typedef struct { const char *name; void *fn; } dm_nvn_ent;
static const dm_nvn_ent g_tab[] = {
    { "nvnDeviceGetInteger",             (void *)nvn_dev_get_integer },
    { "nvnDeviceSetWindowOriginMode",    (void *)nvn_dev_set_origin },
    { "nvnDeviceSetDepthMode",           (void *)nvn_dev_set_depth },
    { "nvnDeviceGetImageHandle",         (void *)nvn_dev_get_image_handle },
    { "nvnDeviceGetSeparateTextureHandle", (void *)nvn_dev_get_tex_handle },
    { "nvnDeviceGetSeparateSamplerHandle", (void *)nvn_dev_get_samp_handle },
    { "nvnDeviceFinalizeCommandHandle",  (void *)nvn_dev_noop },
    { "nvnDeviceInstallDebugCallback",   (void *)nvn_dev_noop },
};

void *dm_nvn_real(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_tab / sizeof g_tab[0]; i++)
        if (!strcmp(g_tab[i].name, name)) return g_tab[i].fn;
    return NULL;
}

// ⭐ nvnBootstrapLoader 的宿主实现（语义见 dm_nvn_ohos.h）。
//   真机实证（2026-10-19）：它是 Volante 图形初始化的第一步——
//   `nvnBootstrapLoader("nvnDeviceInitialize")` / `("nvnDeviceGetProcAddress")`，
//   任一返回 0 ⇒ 初始化在 0x9fc59c 早退 ⇒ 帧循环读 [对象+0x128]（显示对象）崩在 0xa041e0。
static void *(*g_extra_lookup)(const char *name);
void dm_nvn_set_extra(void *(*fn)(const char *name)) { g_extra_lookup = fn; }

void *dm_nvn_bootstrap(const char *name) {
    if (!name || !*name) return NULL;
    void *fn = dm_nvn_real(name);
    if (fn) return fn;
    // M4-a（2026-10-22）：状态族 77 个（纯数据）。放在正式表之后、外部查找之前：
    // 正式表里已有同名实现时不覆盖（先到先得），外部查找是最后兜底。
    fn = dm_nvn_state_lookup(name);
    if (fn) return fn;
    // M4-c（2026-10-22）：设备/窗口/队列（首帧前置）。
    fn = dm_nvn_dev_lookup(name);
    if (fn) return fn;
    // M4-c 第二步（2026-10-22）：命令缓冲记录层 127 个。
    fn = dm_nvn_cb_lookup(name);
    if (fn) return fn;
    if (g_extra_lookup) return g_extra_lookup(name);
    return NULL;
}
