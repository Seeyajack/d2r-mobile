# 第 1 站实验模块：编成 libdmprobe.so，由 ArkTS 侧 import 'libdmprobe.so' 加载。
#
# 这里的配置照抄官方 NDK 的 native 模块写法；真正接入宿主层时，
# 会把 dm/ohos/front_ohos.c 与 dm/host 的平台中立部分一起加进来
# （见 dm/ohos/CMakeLists.txt 里那份待办清单）。
cmake_minimum_required(VERSION 3.5.0)
project(dmprobe)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(dmprobe SHARED
    dm_probe.c
    napi_init.c
)

# 官方要求：用 NAPI 就要链 libace_napi.z.so；打 hilog 要链 libhilog_ndk.z.so
target_link_libraries(dmprobe PUBLIC libace_napi.z.so libhilog_ndk.z.so)
