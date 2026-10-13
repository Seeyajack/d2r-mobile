// NAPI 桥：把实验与能力探测暴露给 ArkTS。
// ArkTS 侧用法：import dmprobe from 'libdmprobe.so';  dmprobe.runProbes()  /  dmprobe.runVkProbe()
#include <napi/native_api.h>
#include <stdlib.h>
#include <string.h>

#include "dm_probe.h"
#include "host_tests.h"
#include "vk_probe.h"

#define PROBE_BUF    16384
#define VKPROBE_BUF  32768   // Vulkan 报告比实验报告长（要列扩展/格式/上限/特性），给大一点
#define TEST_BUF     65536   // 27 个单测的报告 + 失败时的诊断行，给足

// 共用一个"跑一个探针，把报告字符串返回"的骨架，几种探针只差调哪个函数、缓冲区多大。
// Shared skeleton: run a probe and return its report as a string; the probes differ only in which
// function is called and how big the buffer is.
// mode: 0 = 装载实验，1 = Vulkan 能力，2 = 宿主单测（单测要多一个日志路径参数）
static napi_value RunOne(napi_env env, const char *files_dir, int mode) {
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
        // The report is also written to a file under the app data dir, so a later crash cannot lose it.
        char logpath[1200];
        snprintf(logpath, sizeof logpath, "%s/host_tests.log", (files_dir && *files_dir) ? files_dir : ".");
        rc = dm_host_tests_run(logpath, buf, cap);
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

static napi_value RunProbes(napi_env env, napi_callback_info info) {
    // 参数：应用数据目录（ArkTS 传 context.filesDir）。实验 B-3 要在里面造文件试 mmap。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    return RunOne(env, files_dir, 0);
}

static napi_value RunVkProbe(napi_env env, napi_callback_info info) {
    // 图形层能力探测不需要窗口，也不看数据目录（参数留着是为了将来加 surface 阶段时能存日志）。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    return RunOne(env, files_dir, 1);
}

static napi_value RunHostTests(napi_env env, napi_callback_info info) {
    // 宿主单测：报告同时落一份到 filesDir/host_tests.log，崩了也不丢前面的结果。
    char files_dir[1024];
    Arg0String(env, info, files_dir, sizeof files_dir);
    return RunOne(env, files_dir, 2);
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "runProbes",     NULL, RunProbes,     NULL, NULL, NULL, napi_default, NULL },
        { "runVkProbe",    NULL, RunVkProbe,    NULL, NULL, NULL, napi_default, NULL },
        { "runHostTests",  NULL, RunHostTests,  NULL, NULL, NULL, napi_default, NULL },
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
