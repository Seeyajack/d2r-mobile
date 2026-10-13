# 鸿蒙侧原生库构建。
#
# 目标：把 dm/ohos 前端 + dm/host 的宿主 + dm/gfx 图形层，编成 libdmohos.so，
# 由 ArkTS 侧通过 XComponent({ libraryname: 'dmohos' }) 加载。
#
# 当前进度（2026-10-13 深夜）：
#   ① 兼容垫片层 dm/ohos/compat/ 已就位——宿主的苹果头文件全部有同名替身，上游零改动；
#   ② 宿主 71 个 .c 里已有 62 个能**单独编译通过**（用 tools/probe_host.sh 实测，可复现）；
#   ③ 但"单独编译通过"≠"能链接成 .so"：宿主的文件之间互相引用，缺一个就有未定义符号。
#      所以宿主源码仍**整批注释**在这里，等凑齐"能链接的一整批"（把剩下的 6 项依赖处理完）再一次性打开。
#      那 6 项：hle.c(mach-o 段定位)、hle_audio.c(CoreAudio 后端)、hle_lifecycle.c(dispatch+block)、
#      crash.c(信号上下文差异)、hle_fdstat.c(F_GETPATH)、dm_textbox_core.c(指针宽度假设)。
#      另有 3 个文件（diag.c / dm_bright.c / dm_vault_ui.c）需要**你自己的游戏文件**生成的表，属阶段 2。
#
# Current state: the compat shim layer is in place (same-named stand-ins for every Apple header the host
# uses, zero upstream changes), and 62 of the host's 71 .c files already compile individually
# (reproducible via tools/probe_host.sh). Compiling individually is not the same as linking, though --
# the host files reference each other, so the host sources stay commented out below until a whole
# link-complete batch is ready (the remaining 6 dependencies handled; 3 more files need tables generated
# from your own game files, which is stage 2).
cmake_minimum_required(VERSION 3.5.0)
project(dmohos)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(DM_ROOT   ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(DM_OHOS   ${CMAKE_CURRENT_SOURCE_DIR})
set(DM_COMPAT ${DM_OHOS}/compat)

# ---- 兼容垫片：苹果头文件的同名替身（见 compat/README.md）----
set(DM_COMPAT_SOURCES
    ${DM_COMPAT}/src/dm_compat_str.c     # strlcpy / strlcat / arc4random / renamex_np
    ${DM_COMPAT}/src/dm_compat_hash.c    # MD5 / SHA-1 / SHA-256（CommonCrypto）
    ${DM_COMPAT}/src/dm_compat_mach.c    # 时钟 / 读内存 / 内存统计 + 线程状态的诚实降级
    ${DM_COMPAT}/src/dm_compat_fs.c      # sysctl / clonefile / xattr / malloc 统计
)

set(DM_SOURCES
    ${DM_OHOS}/front_ohos.c              # front.h 契约的鸿蒙实现
    ${DM_OHOS}/dm_log.c                  # 统一日志出口（LOG → hilog）
    ${DM_COMPAT_SOURCES}

    # ---- 宿主（dm/host）：等"能链接的一整批"凑齐后再逐个打开 ----
    # ${DM_ROOT}/host/main.c
    # ${DM_ROOT}/host/loader.c
    # ${DM_ROOT}/host/hle.c
    # ${DM_ROOT}/host/hle_svc.c
    # ${DM_ROOT}/host/hle_thread.c
    # ${DM_ROOT}/host/hle_fs.c
    # ...（全部 71 个，按批 1 / 批 2 / 批 3 分次加入）
    # ${DM_ROOT}/host/entry.S            # 闸门入口（汇编）

    # ---- 图形层（dm/gfx）：暂不碰 ----
    # ${DM_ROOT}/gfx/nvn/hle_nvn.c
    # ${DM_ROOT}/gfx/nvn/nvn_vk.c        # 鸿蒙侧新写（替代 nvn_mtl.m）
)

add_library(dmohos SHARED ${DM_SOURCES})

# 注意 BEFORE：垫片目录必须排在 sysroot 之前，否则 #include <TargetConditionals.h> 之类会先去找系统目录、
# 找不到再报错——那样"零改动替换"就不成立了。
# BEFORE matters: the shim directory must precede the sysroot, otherwise the Apple-style includes would
# look in the system directories first and fail, which would break the zero-change property.
target_include_directories(dmohos BEFORE PRIVATE
    ${DM_COMPAT}
    ${DM_OHOS}
    ${DM_ROOT}/host
    ${DM_ROOT}/gfx
)

target_compile_definitions(dmohos PRIVATE
    __OHOS__=1              # 显式声明平台，不只依赖编译器三元组
    DM_LOG_TO_HILOG=1       # dm.h 的 LOG 宏改走 hilog（见 dm/ohos/dm_log.h）
    _GNU_SOURCE=1           # musl 靠它放出一批 BSD/GNU 扩展（如 memmem、dladdr）——
                            # 苹果的 <string.h> 里这些是默认就有的，所以上游直接用了
                            # memmem 却没包含任何额外头；鸿蒙不定义这个宏就会"隐式声明"
)

# shim_prefix.h：强制包含进每个编译单元。这里用相对名而不是绝对路径——
# clang 的 -include 按"引号包含"的方式查找，会走 -I 目录，于是绕开了
# 工程路径带空格（"HarmonyOS NEXT Porting Project"）带来的转义麻烦。
# shim_prefix.h is force-included into every translation unit. A relative name is used rather than an
# absolute path because clang resolves -include like a quoted include and therefore searches the -I
# directories -- sidestepping the escaping trouble from the spaces in this project's path.
target_compile_options(dmohos PRIVATE
    -include shim_prefix.h
    -fno-common
    -Wall
)

# 官方文档要求链接的库：
#   libace_napi.z.so    —— Node-API（ArkTS ↔ C++ 桥）
#   libace_ndk.z.so     —— ArkUI 的 NDK 接口
#   libnative_window.so —— OHNativeWindow（Vulkan 的 VkSurfaceKHR 要对接到它）
#   libhilog_ndk.z.so   —— hilog（dm_log.c 用）
target_link_libraries(dmohos PUBLIC
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libhilog_ndk.z.so
    pthread
)
