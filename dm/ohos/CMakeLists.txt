# 鸿蒙侧原生库构建（骨架，尚未验证）。
#
# 目标：把 dm/ohos 前端 + dm/host 的平台中立部分 + dm/gfx 图形层，编成 libdmohos.so，
# 由 ArkTS 侧通过 import dmohos from 'libdmohos.so' 加载。
#
# 现状：只挂了前端一个源文件。宿主其余部分要等"适配清单"里的 8 类苹果专属依赖
# 逐类替换完之后再逐步加进来（见 ../ohos/README.md）。
cmake_minimum_required(VERSION 3.5.0)
project(dmohos)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DM_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

set(DM_SOURCES
    ${DM_ROOT}/ohos/front_ohos.c
    # 待加入（按"适配清单-宿主层.md"的顺序替换后逐个打开）：
    # ${DM_ROOT}/host/main.c
    # ${DM_ROOT}/host/loader.c
    # ${DM_ROOT}/host/hle.c
    # ${DM_ROOT}/host/hle_svc.c
    # ${DM_ROOT}/host/hle_thread.c
    # ${DM_ROOT}/host/hle_fs.c
    # ...
    # ${DM_ROOT}/gfx/nvn/hle_nvn.c
    # ${DM_ROOT}/gfx/nvn/nvn_vk.c      ← 鸿蒙侧新写（替代 nvn_mtl.m）
)

add_library(dmohos SHARED ${DM_SOURCES})

target_include_directories(dmohos PRIVATE
    ${DM_ROOT}/host
    ${DM_ROOT}/ohos
    ${DM_ROOT}/gfx
)

# 官方文档要求链接的库：
#   libace_napi.z.so   —— Node-API（ArkTS ↔ C++ 桥）
#   libace_ndk.z.so    —— ArkUI 的 NDK 接口
#   libnative_window.so—— OHNativeWindow（Vulkan 的 VkSurfaceKHR 要对接到它）
target_link_libraries(dmohos PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    pthread
)

# 注意：宿主源码目前直接 #include 了苹果头文件（mach-o、TargetConditionals 等）。
# 在这些依赖被替换之前，本 CMakeLists 无法成功编译——这是预期的。
