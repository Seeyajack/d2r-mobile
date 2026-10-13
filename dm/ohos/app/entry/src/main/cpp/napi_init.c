// NAPI 桥：把三个实验暴露给 ArkTS。
// ArkTS 侧用法：import dmprobe from 'libdmprobe.so';  dmprobe.runProbes()
#include <napi/native_api.h>
#include <stdlib.h>

#include "dm_probe.h"

#define PROBE_BUF 16384

static napi_value RunProbes(napi_env env, napi_callback_info info) {
    // 参数：应用数据目录（ArkTS 传 context.filesDir）。实验 B-3 要在里面造文件试 mmap。
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
    char files_dir[1024] = { 0 };
    if (argc > 0 && argv[0]) {
        size_t len = 0;
        napi_get_value_string_utf8(env, argv[0], files_dir, sizeof files_dir, &len);
    }

    napi_value ret = NULL;

    char *buf = (char *)malloc(PROBE_BUF);
    if (!buf) {
        napi_create_string_utf8(env, "内存不足，实验未运行", NAPI_AUTO_LENGTH, &ret);
        return ret;
    }
    buf[0] = 0;

    int rc = dm_probe_run(files_dir, buf, PROBE_BUF);
    if (rc != 0 && buf[0] == 0) {
        napi_create_string_utf8(env, "实验入口参数异常", NAPI_AUTO_LENGTH, &ret);
        free(buf);
        return ret;
    }

    napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &ret);
    free(buf);
    return ret;
}

static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "runProbes", NULL, RunProbes, NULL, NULL, NULL, napi_default, NULL },
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
