// NAPI 桥：把实验与能力探测暴露给 ArkTS。
// ArkTS 侧用法：import dmprobe from 'libdmprobe.so';  dmprobe.runProbes()  /  dmprobe.runVkProbe()
#include <napi/native_api.h>
#include <ace/xcomponent/native_interface_xcomponent.h>
#include <stdio.h>          // snprintf：拼单测日志路径要用（少了它只是隐式声明，编译器当它返回 int）
#include <stdlib.h>
#include <string.h>

#include "dm_probe.h"
#include "host_tests.h"
#include "vk_probe.h"
#include "vk_backend.h"
#include "vk_surface_ohos.h"

// ---- XComponent 生命周期：把 ArkUI 给的 OHNativeWindow 接住（出图的地基）----
// 窗口由 ArkUI 持有，系统管生命周期；我们只接住并在销毁时清空。
static void OnSurfaceCreatedCB(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0, h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) { w = h = 0; }
    dm_ohos_surface_set_window(window, (uint32_t)w, (uint32_t)h);
}
static void OnSurfaceChangedCB(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0, h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) { w = h = 0; }
    dm_ohos_surface_set_window(window, (uint32_t)w, (uint32_t)h);
}
static void OnSurfaceDestroyedCB(OH_NativeXComponent *component, void *window) {
    (void)component; (void)window;
    dm_ohos_surface_clear_window();
}
static void DispatchTouchEventCB(OH_NativeXComponent *component, void *window) {
    (void)component; (void)window;   // 探针不需要触摸
}

#define PROBE_BUF    16384
#define VKPROBE_BUF  32768   // Vulkan 报告比实验报告长（要列扩展/格式/上限/特性），给大一点
#define TEST_BUF     65536   // 单测的报告 + 失败时的诊断行，给足
#define VKSMOKE_BUF  16384   // Vulkan 冒烟报告（设备名 + 逐项结论）
#define VKPACE_BUF   32768   // 节拍报告：5 组对照 + 判据，比冒烟长得多

// 共用一个"跑一个探针，把报告字符串返回"的骨架，几种探针只差调哪个函数、缓冲区多大。
// Shared skeleton: run a probe and return its report as a string; the probes differ only in which
// function is called and how big the buffer is.
// mode: 0 = 装载实验，1 = Vulkan 能力，2 = 宿主单测（单测要多一个日志路径与筛选参数）
static napi_value RunOne(napi_env env, const char *files_dir, const char *filter, int mode) {
    napi_value ret = NULL;
    size_t cap = mode == 1 ? VKPROBE_BUF : (mode == 2 ? TEST_BUF : PROBE_BUF);
    char *buf = (char *)malloc(cap);
    if (!buf) {
        napi_create_string_utf8(env, "内存不足，探针未运行", NAPI_AUTO_LENGTH, &ret);
        return ret;
    }
    buf[0] = 0;

    int rc;
    if (mode == 1) {
        rc = dm_vk_probe_run(buf, cap);
    } else if (mode == 2) {
        // 结果同时落到应用数据目录下的一个文件里：即使某个单测把进程搞崩，前面的结果也在盘上。
        char logpath[1200];
        snprintf(logpath, sizeof logpath, "%s/host_tests.log", (files_dir && *files_dir) ? files_dir : ".");
        rc = dm_host_tests_run(logpath, filter, buf, cap);
    } else {
        rc = dm_probe_run(files_dir, buf, cap);
    }
    if (rc != 0 && buf[0] == 0) {
        napi_create_string_utf8(env, "探针入口参数异常", NAPI_AUTO_LENGTH, &ret);
        free(buf);
        return ret;
    }

    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

// 取第 0 个参数（应用数据目录）
static void Arg0String(napi_env env, napi_callback_info info, char *out, size_t cap) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    out[0] = 0;
    if (argc > 0 && argv[0]) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], out, cap, &len);
    }
}

// 取第 1 个参数（单测筛选），没有就给空串
static void Arg1String(napi_env env, napi_callback_info info, char *out, size_t cap) {
    size_t argc = 2;
    napi_value argv[2] = { NULL, NULL };
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    out[0] = 0;
    if (argc > 1 && argv[1]) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[1], out, cap, &len);
    }
}

static napi_value RunProbes(napi_env env, napi_callback_info info) {
    // 参数：应用数据目录（ArkTS 传 context.filesDir）。实验 B-3 要在里面造文件试 mmap。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    return RunOne(env, files_dir, "", 0);
}

static napi_value RunVkProbe(napi_env env, napi_callback_info info) {
    // 图形层能力探测不需要窗口，也不看数据目录（参数留着是为了将来加 surface 阶段时能存日志）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    return RunOne(env, files_dir, "", 1);
}

static napi_value RunHostTests(napi_env env, napi_callback_info info) {
    // 宿主单测：报告同时落一份到 filesDir/host_tests.log，崩了也不丢前面的结果。
    // 第 2 个参数是筛选（"" = 续跑；"*" = 清日志重头跑；单测名 = 只跑它）——查问题时用得上。
    char files_dir[1024], filter[128];
    Arg0String(env, info, files_dir, sizeof files_dir);
    Arg1String(env, info, filter, sizeof filter);
    return RunOne(env, files_dir, filter, 2);
}

static napi_value RunVkSmoke(napi_env env, napi_callback_info info) {
    // 图形层阶段 1：Vulkan 离屏冒烟（清屏 + 读回逐像素验证），不依赖窗口与游戏文件。
    // 参数留位（将来加 surface 阶段时要存日志），现在不看。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_smoke(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkShader(napi_env env, napi_callback_info info) {
    // 图形层阶段 2：SPIR-V 直通（手写 SPIR-V 着色器画三角形 + 读回验证）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_shader(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkMemProbe(napi_env env, napi_callback_info info) {
    // 阶段 3 前置：内存池零拷贝的平台能力验证（规模 + 机制）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_memprobe(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkPoolProto(napi_env env, napi_callback_info info) {
    // 阶段 3 第一步原型（3.1-A）：池零拷贝（模拟客体堆 + 池切片 + 设备地址线性 + GPU 读写）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_poolproto(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkHeapReserve(napi_env env, napi_callback_info info) {
    // 阶段 3 第一步（3.1-B）：客体堆预留（真实尺寸 4 GB）+ 池切片可被 GPU 读写。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_heapreserve(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkPoolMod(napi_env env, napi_callback_info info) {
    // 阶段 3 第一步（3.1-C）：池模块（nvn_vk.c）—— 登记 / 地址反查 / GPU 搬运 / 负向判据。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_poolmod(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkReplay(napi_env env, napi_callback_info info) {
    // 阶段 3 第二步（3.2）：回放层最小闭环（手写命令流 → 回放 → 逐像素验证）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_replay(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkPsoCache(napi_env env, napi_callback_info info) {
    // 管线缓存落盘：冷启动 vs 热启动，比建管线耗时。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_psocache(files_dir, buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkOnScreen(napi_env env, napi_callback_info info) {
    // 阶段 4（3.3-A）：出图 —— XComponent 窗口 → VkSurfaceKHR → swapchain → present。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKSMOKE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_onscreen(buf, VKSMOKE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkPace(napi_env env, napi_callback_info info) {
    // 阶段 4（3.3-B）：可变帧率节拍 —— OH_DisplaySoloist 当报时员，5 组对照（含裸 present 基线）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKPACE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_pace(buf, VKPACE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value RunVkPaceXc(napi_env env, napi_callback_info info) {
    // 阶段 4（3.3-C）：帧率接入口对照 —— DisplaySoloist vs XComponent 侧，6 组同轮。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    napi_value ret = NULL;
    char *buf = (char *)malloc(VKPACE_BUF);
    if (!buf) { napi_create_string_utf8(env, "内存不足", NAPI_AUTO_LENGTH, &ret); return ret; }
    vk_backend_pace_xc(buf, VKPACE_BUF);
    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value Init(napi_env env, napi_value exports) {
    // 取 ArkUI 传进来的 XComponent 实例并注册回调（照 dm/ohos/front_ohos.c 的写法）
    napi_value exportInstance = NULL;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &exportInstance) == napi_ok) {
        OH_NativeXComponent *xc = NULL;
        if (napi_unwrap(env, exportInstance, (void **)&xc) == napi_ok && xc) {
            static OH_NativeXComponent_Callback cb;
            cb.OnSurfaceCreated = OnSurfaceCreatedCB;
            cb.OnSurfaceChanged = OnSurfaceChangedCB;
            cb.OnSurfaceDestroyed = OnSurfaceDestroyedCB;
            cb.DispatchTouchEvent = DispatchTouchEventCB;
            OH_NativeXComponent_RegisterCallback(xc, &cb);
            // 把实例也存一份：官方"可变帧率"在 XComponent 侧另有一套接口
            // （SetExpectedFrameRateRange + RegisterOnFrameCallback），3.3-C 要拿它和 DisplaySoloist 对照。
            dm_ohos_surface_set_xc(xc);
        }
    }

    napi_property_descriptor desc[] = {
        { "runProbes",      NULL, RunProbes,      NULL, NULL, NULL, napi_default, NULL },
        { "runVkProbe",     NULL, RunVkProbe,     NULL, NULL, NULL, napi_default, NULL },
        { "runHostTests",   NULL, RunHostTests,   NULL, NULL, NULL, napi_default, NULL },
        { "runVkSmoke",     NULL, RunVkSmoke,     NULL, NULL, NULL, napi_default, NULL },
        { "runVkShader",    NULL, RunVkShader,    NULL, NULL, NULL, napi_default, NULL },
        { "runVkMemProbe",  NULL, RunVkMemProbe,  NULL, NULL, NULL, napi_default, NULL },
        { "runVkPoolProto", NULL, RunVkPoolProto, NULL, NULL, NULL, napi_default, NULL },
        { "runVkHeapReserve", NULL, RunVkHeapReserve, NULL, NULL, NULL, napi_default, NULL },
        { "runVkPoolMod",   NULL, RunVkPoolMod,   NULL, NULL, NULL, napi_default, NULL },
        { "runVkReplay",    NULL, RunVkReplay,    NULL, NULL, NULL, napi_default, NULL },
        { "runVkPsoCache",  NULL, RunVkPsoCache,  NULL, NULL, NULL, napi_default, NULL },
        { "runVkOnScreen",  NULL, RunVkOnScreen,  NULL, NULL, NULL, napi_default, NULL },
        { "runVkPace",      NULL, RunVkPace,      NULL, NULL, NULL, napi_default, NULL },
        { "runVkPaceXc",    NULL, RunVkPaceXc,    NULL, NULL, NULL, napi_default, NULL },
    };
    napi_define_properties(env, exports, sizeof desc / sizeof desc[0], desc);
    return exports;
}

static napi_module g_dmprobe_module = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = NULL,
    .nm_register_func = Init,
    .nm_modname = "dmprobe",          // ArkTS：import dmprobe from 'libdmprobe.so'
    .nm_priv = NULL,
    .reserved = { 0 },
};

__attribute__((constructor)) static void DmProbeRegister(void) {
    napi_module_register(&g_dmprobe_module);
}
