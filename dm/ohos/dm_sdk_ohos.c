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

// ═══════════ nn::os 事件 / 轻量事件 / 信号量（2026-10-19 np130 补）═══════════
// 真机实证：这三族**一个都没实现**（全落通用桩、返 0）——而"等事件"是任何 Horizon 程序的骨架：
//   SignalLightEvent 被调 256 次却什么都没发生；Wait* 立刻返回"成功"，上层只能空转或判错。
//   与 libc 空桩同性质、同样与游戏无关（换游戏照用），所以放这层。
// 语义按 nnSdk：
//   Event / LightEvent：有"已置位"标志；ClearMode 0=自动复位（被取走即清）、1=手动复位。
//     Wait 阻塞到置位为止，TryWait 不阻塞，TimedWait 带超时。
//   Semaphore：计数；Acquire 为 0 时阻塞，Release 加一（也可指定个数）。
#include <pthread.h>
#include <time.h>

#define DM_EV_MAX 512
typedef struct {
    void            *key;
    int32_t          val;        // 事件：1=已置位；信号量：当前计数
    int32_t          manual;     // 1 = 手动复位
    int32_t          is_sem;
    pthread_mutex_t  mu;
    pthread_cond_t   cv;
} dm_ev_slot;
static dm_ev_slot g_ev[DM_EV_MAX];
static pthread_mutex_t g_ev_reg = PTHREAD_MUTEX_INITIALIZER;
static int g_n_evfull;

static dm_ev_slot *ev_get(void *k, int is_sem) {
    pthread_mutex_lock(&g_ev_reg);
    for (int i = 0; i < DM_EV_MAX; i++) if (g_ev[i].key == k) { pthread_mutex_unlock(&g_ev_reg); return &g_ev[i]; }
    for (int i = 0; i < DM_EV_MAX; i++) if (!g_ev[i].key) {
        g_ev[i].key = k; g_ev[i].val = 0; g_ev[i].manual = 1; g_ev[i].is_sem = is_sem;
        pthread_mutex_init(&g_ev[i].mu, NULL);
        pthread_condattr_t ca; pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_init(&g_ev[i].cv, &ca);
        pthread_condattr_destroy(&ca);
        pthread_mutex_unlock(&g_ev_reg);
        return &g_ev[i];
    }
    pthread_mutex_unlock(&g_ev_reg);
    if (++g_n_evfull <= 8) SDKLOG("【红】事件表满（%d 格）！%p 退化到槽 0\n", DM_EV_MAX, k);
    return &g_ev[0];
}

static uint64_t os_init_event(uint64_t a0, uint64_t signalled, uint64_t mode) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 0);
    e->manual = (int32_t)mode;              // EventClearMode: 0=auto, 1=manual
    e->val = (int32_t)(signalled & 1);
    return 0;
}
static uint64_t os_signal_event(uint64_t a0) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu); e->val = 1; pthread_cond_broadcast(&e->cv); pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_clear_event(uint64_t a0) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu); e->val = 0; pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_try_wait_event(uint64_t a0) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu);
    int hit = e->val != 0;
    if (hit && !e->manual) e->val = 0;
    pthread_mutex_unlock(&e->mu);
    return (uint64_t)hit;
}
static uint64_t os_finalize_event(uint64_t a0) { (void)a0; return 0; }

// 轻量事件：同一套语义，只是另一组类型名
static uint64_t os_wait_light(uint64_t a0) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 0);
    pthread_mutex_lock(&e->mu);
    while (!e->val) pthread_cond_wait(&e->cv, &e->mu);
    if (!e->manual) e->val = 0;
    pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_timedwait_light(uint64_t a0, uint64_t ns) {   // TimeSpan 按值传在 x1
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 0);
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    int64_t n = (int64_t)ns;
    if (n < 0) n = 0;
    if (n == 0) return os_try_wait_event(a0);   // 0 超时 = 不阻塞
    dl.tv_sec += n / 1000000000; dl.tv_nsec += n % 1000000000;
    if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&e->mu);
    while (!e->val) if (pthread_cond_timedwait(&e->cv, &e->mu, &dl) != 0) break;
    int hit = e->val != 0;
    if (hit && !e->manual) e->val = 0;
    pthread_mutex_unlock(&e->mu);
    return (uint64_t)hit;
}

// 信号量
static uint64_t os_init_sem(uint64_t a0, uint64_t init, uint64_t maxv) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 1);
    (void)maxv;
    e->val = (int32_t)init;
    return 0;
}
static uint64_t os_acquire_sem(uint64_t a0) {
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 1);
    pthread_mutex_lock(&e->mu);
    while (e->val <= 0) pthread_cond_wait(&e->cv, &e->mu);
    e->val--;
    pthread_mutex_unlock(&e->mu);
    return 0;
}
static uint64_t os_release_sem(uint64_t a0, uint64_t n) {   // 单参重载时 x1 是脏值，按 ≥1 处理
    dm_ev_slot *e = ev_get((void *)(uintptr_t)a0, 1);
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
    int (*fn)(uint64_t, uint64_t, uint64_t, uint64_t);
} dm_sdk_ent;

#define E(name, f) { name, (int (*)(uint64_t, uint64_t, uint64_t, uint64_t))(f) }

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
    // ---- nn::os：时基与睡眠 ----
    E("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE", os_convert_timespan),
    E("_ZN2nn2os11SleepThreadENS_8TimeSpanE", os_sleep),
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
