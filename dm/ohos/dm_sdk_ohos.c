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

// ═══════════════ nn::os：时基换算与睡眠（2026-10-19 np78 补）═══════════════
// 为什么补这两个：`ConvertToTimeSpan` 此前是"一律返 0"的桩 ⇒ 游戏算出的睡眠时长恒为 0
//   ⇒ 主循环**全速空转**（真机实测 18578 帧、日志刷 3 万行，把诊断信号全淹掉）。
//   换成真换算后，循环按游戏本来的帧率跑（60 帧左右），时序逻辑也恢复。
// 换算基准与探针里的 GetSystemTick 一致：**19.2MHz 时基**（Switch 约定）
//   ⇒ 1 tick = 1000/19.2 ns = 625/12 ns（用整数乘除避免浮点）。
#include <time.h>

#define DM_TICK_HZ 19200000ull    // 19.2 MHz

static uint64_t os_convert_timespan(uint64_t tick) {
    // nn::os::ConvertToTimeSpan(Tick) -> TimeSpan（{int64 ns}，按值返回 ⇒ x0）
    return (uint64_t)tick * 625u / 12u;
}
static uint64_t os_sleep(uint64_t ns) {
    if (!ns) return 0;
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
    // ---- nn::os：时基与睡眠 ----
    E("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE", os_convert_timespan),
    E("_ZN2nn2os11SleepThreadENS_8TimeSpanE", os_sleep),
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
