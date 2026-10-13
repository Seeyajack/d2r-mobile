// 鸿蒙前端实现（设计骨架）。与 front_mac.m 的对应关系：
//   front_run          → Mac 开 NSWindow + CAMetalLayer + [NSApp run]（阻塞）
//                        鸿蒙：表面由 ArkUI 的 XComponent 提供，本函数只把游戏挂到独立线程上，随即返回
//   front_metal_layer  → Mac 返回 CAMetalLayer；鸿蒙返回 NULL（改用 front_ohos_native_window）
//   front_game_res     → 纯算术，与平台无关，原样照抄 front_mac.m
//   front_text_input   → Mac 弹 NSAlert；鸿蒙要调 ArkTS 对话框（待接）
//
// 尚未编译验证。现状与待办见 dm/ohos/README.md。
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>

#include "dm.h"
#include "front.h"
#include "front_ohos.h"

// ───────────────────────── 静态状态 ─────────────────────────

static OH_NativeXComponent *g_xc;          // ArkUI 给的 XComponent 实例
static void *g_window;                     // 当前 OHNativeWindow（系统持有引用，销毁后必须清空）
static int g_sw, g_sh;                     // 表面尺寸（像素）
static int g_guest_running;

void *front_ohos_native_window(void) { return g_window; }

void front_ohos_surface_size(int *w, int *h) {
    if (w) *w = g_sw;
    if (h) *h = g_sh;
}

// ─────────────────── XComponent 生命周期回调 ───────────────────

static void OnSurfaceCreatedCB(OH_NativeXComponent *component, void *window) {
    g_window = window;                     // window 就是 OHNativeWindow*
    uint64_t w = 0, h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) == OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        g_sw = (int)w; g_sh = (int)h;
    }
    LOG("鸿蒙前端：Surface 创建 %dx%d", g_sw, g_sh);
    // TODO(阶段 1)：在这里通知图形后端用 g_window 创建 VkSurfaceKHR。
}

static void OnSurfaceChangedCB(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0, h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) == OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        g_sw = (int)w; g_sh = (int)h;
        LOG("鸿蒙前端：Surface 尺寸变化 %dx%d", g_sw, g_sh);
    }
    // TODO(阶段 1)：图形后端重建交换链。
}

static void OnSurfaceDestroyedCB(OH_NativeXComponent *component, void *window) {
    (void)component; (void)window;
    // 官方文档：销毁后系统会把 OHNativeWindow 的引用减一，可能就此释放。
    // 所以这里必须清空，绝不能留旧指针给图形后端用。
    g_window = NULL; g_sw = g_sh = 0;
    LOG("鸿蒙前端：Surface 销毁");
    // TODO(阶段 1)：图形后端销毁 VkSurfaceKHR。
}

// 触摸：ArkUI 一次派发一个事件，事件里带"当前所有触点"。
// 转成 front.h 的 dm_touch（归一化坐标，左上为原点），整组推给宿主。
static void DispatchTouchEventCB(OH_NativeXComponent *component, void *window) {
    OH_NativeXComponent_TouchEvent ev;
    if (OH_NativeXComponent_GetTouchEvent(component, window, &ev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) return;

    // 抬起 / 取消 / 没有触点 → 整组清空（front.h：n = 0 = 全抬起）
    if (ev.numPoints == 0 || ev.type == OH_NATIVEXCOMPONENT_UP || ev.type == OH_NATIVEXCOMPONENT_CANCEL) {
        dm_touch_set(NULL, 0);
        return;
    }
    if (g_sw <= 0 || g_sh <= 0) return;

    dm_touch ts[16];                       // front.h：手指槽号 0–15
    int n = 0;
    for (uint32_t i = 0; i < ev.numPoints && n < 16; i++) {
        const OH_NativeXComponent_TouchPoint *p = &ev.touchPoints[i];
        ts[n].id = (uint32_t)p->id;
        ts[n].x = (float)p->x / (float)g_sw;   // p->x/y 相对组件左上角
        ts[n].y = (float)p->y / (float)g_sh;
        n++;
    }
    dm_touch_set(ts, n);
}

// ─────────────────────── front.h 契约实现 ───────────────────────

// 鸿蒙没有 Metal：本函数只用于满足 front.h 契约，恒返回 NULL。
// 图形后端请改用 front_ohos_native_window()。
void *front_metal_layer(void) { return NULL; }

// 与 front_mac.m 完全一致：原版 16:9 常数，DM_GAME_RES 只供与 iOS 宽屏对拍。
uint64_t front_game_res(int docked) {
    unsigned w, h; const char *e = getenv("DM_GAME_RES");
    if (e && sscanf(e, "%ux%u", &w, &h) == 2) return docked ? (uint64_t)h << 32 | w : (uint64_t)(h * 4 / 5) << 32 | (w * 4 / 5 + 3) / 4 * 4;
    return docked ? 900ull << 32 | 1600 : 720ull << 32 | 1280;
}

typedef struct { void (*fn)(void); } guest_arg;

static void *guest_tramp(void *p) {
    ((guest_arg *)p)->fn();
    LOG("鸿蒙前端：游戏线程结束");
    return NULL;
}

// 与 Mac 不同：不阻塞。ArkUI 的 UI 线程要继续跑，所以只把游戏挂到独立线程上。
// 栈 64 MiB：与 front_mac.m 一致（Switch 主线程栈由 npdm 指定，宿主给足）。
void front_run(void (*guest)(void)) {
    if (g_guest_running) { LOG("鸿蒙前端：已在运行，忽略重复 front_run"); return; }
    g_guest_running = 1;
    static guest_arg ga; ga.fn = guest;
    pthread_attr_t a; pthread_attr_init(&a);
    if (pthread_attr_setstacksize(&a, 64u << 20)) LOG("【红】鸿蒙前端：设 64 MiB 栈失败，按默认栈跑");
    pthread_t th;
    if (pthread_create(&th, &a, guest_tramp, &ga)) { LOG("【红】鸿蒙前端：pthread_create 失败"); g_guest_running = 0; return; }
    pthread_detach(th);
}

// TODO(阶段 4)：软键盘输入框。需要经 NAPI 回调到 ArkTS 弹对话框，并阻塞等结果（与 Mac 的语义一致）。
// 现在先按"无窗口"语义返回 -1，调用方（hle_swkbd.c）会当成取消处理。
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)initial; (void)max_chars; (void)out; (void)out_cap;
    LOG("鸿蒙前端：软键盘尚未接入，本次输入按取消处理");
    return -1;
}

// ───────────────────── ArkTS → 原生 的入口 ─────────────────────

// 入口：读完 dm/host/main.c 后确认——它的 main() 不是 static，而且只做三件事：
//   ① 预载（host_preload，或等 dm_host_preload_start 起的后台线程）
//   ② front_run(guest_main)
//   ③ return 0        ← 不调 exit()，不会杀掉进程
// 而本文件的 front_run 不阻塞（只把游戏挂到线程上就返回），
// 所以鸿蒙侧【可以直接调 main()，不需要改 main.c】。
//
// 唯一前提：main.c 第 170 行 #include <TargetConditionals.h> 和第 173 行 #if TARGET_OS_OSX，
// 鸿蒙上没有这个头 —— 按「适配清单-宿主层.md」第 1 项处理（去掉该分支或提供平台宏）即可。
//
// 若链接器不接受共享库里出现名为 main 的符号，就在 main.c 加一个薄包装（如 dm_host_main_ohos）转调。
extern int main(int argc, char **argv);

static int boot_started;

int dm_ohos_boot(const char *mods, const char *romfs, const char *save, const char *temp) {
    if (boot_started) { LOG("鸿蒙前端：已启动过，忽略"); return 0; }
    if (!mods || !*mods || !romfs || !*romfs || !save || !*save) {
        LOG("【红】鸿蒙前端：mods / romfs / save 都不能为空"); return -1;
    }
    boot_started = 1;

    setenv("DM_ROMFS", romfs, 1);
    setenv("DM_SAVE", save, 1);            // 上游要求 DM_SAVE 必给
    if (temp && *temp) setenv("DM_TEMP", temp, 1);

    // 上游在 main() 开头就把 cwd 设到工作目录；鸿蒙侧同样要有确定的 cwd。
    if (chdir(save) != 0) LOG("【黄】鸿蒙前端：chdir(%s) 失败，沿用当前 cwd", save);

    // argv 的形状照 main.c：argv[0] = 程序名，argv[1] = 模块目录（loader.c 的 read_layout /
    // register_gates 从这里读 *.fixups.tsv / *.gates.tsv），其后是模块名，"--" 之后是游戏参数。
    static char a0[] = "dmhost", a2[] = "main", a3[] = "sdk", mods_copy[1024];
    snprintf(mods_copy, sizeof mods_copy, "%s", mods);
    static char *av[] = { a0, mods_copy, a2, a3 };
    const int ac = (int)(sizeof av / sizeof av[0]);

    // 宿主装载提前起跑（与 iOS 壳一致：UI 起来之前先叫它跑）。
    dm_host_preload_start(ac, av);

    // 进入游戏主流程：main() 会等预载完成，再调 front_run(guest_main) 把游戏挂到独立线程上，随即返回。
    int r = main(ac, av);
    if (r) LOG("【红】鸿蒙前端：主流程返回 %d", r);
    return r;
}

// ─────────────────────── NAPI 模块注册 ───────────────────────

static char *js_str(napi_env env, napi_value v, char *buf, size_t cap) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, buf, cap, &len) != napi_ok) buf[0] = 0;
    return buf;
}

static napi_value JsBoot(napi_env env, napi_callback_info info) {
    size_t argc = 4; napi_value argv[4] = { NULL, NULL, NULL, NULL };
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char mods[1024] = {0}, romfs[1024] = {0}, save[1024] = {0}, temp[1024] = {0};
    if (argc > 0 && argv[0]) js_str(env, argv[0], mods, sizeof mods);
    if (argc > 1 && argv[1]) js_str(env, argv[1], romfs, sizeof romfs);
    if (argc > 2 && argv[2]) js_str(env, argv[2], save, sizeof save);
    if (argc > 3 && argv[3]) js_str(env, argv[3], temp, sizeof temp);
    int r = dm_ohos_boot(mods, romfs, save, temp);
    napi_value ret; napi_create_int32(env, r, &ret); return ret;
}

static napi_value DmOhosInit(napi_env env, napi_value exports) {
    // 取 ArkUI 传进来的 XComponent 实例并注册回调
    napi_value exportInstance = NULL;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) == napi_ok) {
        OH_NativeXComponent *xc = NULL;
        if (napi_unwrap(env, exportInstance, (void **)&xc) == napi_ok && xc) {
            g_xc = xc;
            static OH_NativeXComponent_Callback cb;
            cb.OnSurfaceCreated = OnSurfaceCreatedCB;
            cb.OnSurfaceChanged = OnSurfaceChangedCB;
            cb.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
            cb.DispatchTouchEvent = DispatchTouchEventCB;
            OH_NativeXComponent_RegisterCallback(xc, &cb);
        } else {
            LOG("【红】鸿蒙前端：napi_unwrap 取 XComponent 失败");
        }
    }

    napi_property_descriptor desc[] = {
        { "boot", NULL, JsBoot, NULL, NULL, NULL, napi_default, NULL },
    };
    napi_define_properties(env, exports, sizeof desc / sizeof desc[0], desc);
    return exports;
}

static napi_module g_dmohos_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = NULL,
    .nm_register_func = DmOhosInit,
    .nm_modname = "dmohos",              // ArkTS 侧：import dmohos from 'libdmohos.so'
    .nm_priv = NULL,
    .reserved = { 0 },
};

__attribute__((constructor)) static void DmOhosModuleRegister(void) {
    napi_module_register(&g_dmohos_module);
}
