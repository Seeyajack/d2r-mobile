// 通用 SDK 胶水层实现（见头文件顶部的定位说明）。
// Implementation of the general SDK glue layer (see the header for the rationale).
#include "dm_sdk_ohos.h"

#include <stddef.h>
#include <string.h>

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

// ═══════════════ 分派表 ═══════════════
// 约定：全部按"最多 3 个入参 + 32 位返回值"调用（足够覆盖这批轻量接口）。
// 需要更多入参的（如 CreateLayer 的第二参）已在调用点取出；返回 64 位的接口另开分支。
typedef struct {
    const char *sym;
    int (*fn)(uint64_t, uint64_t, uint64_t);
} dm_sdk_ent;

#define E(name, f) { name, (int (*)(uint64_t, uint64_t, uint64_t))(f) }

static const dm_sdk_ent g_tab[] = {
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
};

int dm_sdk_call(const char *sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t *ret) {
    if (!sym) return 0;
    for (size_t i = 0; i < sizeof g_tab / sizeof g_tab[0]; i++)
        if (!strcmp(g_tab[i].sym, sym)) {
            int r = g_tab[i].fn(a0, a1, a2);
            if (ret) *ret = (uint64_t)(uint32_t)r;   // Result / 枚举都是 32 位
            return 1;
        }
    return 0;
}
