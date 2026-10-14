# 第 1 站实验模块：编成 libdmprobe.so，由 ArkTS 侧 import 'libdmprobe.so' 加载。
#
# 这里的配置照抄官方 NDK 的 native 模块写法；真正接入宿主层时，
# 会把 dm/ohos/front_ohos.c 与 dm/host 的平台中立部分一起加进来
# （见 dm/ohos/CMakeLists.txt 里那份待办清单）。
cmake_minimum_required(VERSION 3.5.0)
project(dmprobe)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 汇编（test_image.S）要显式启用 ASM 语言
enable_language(ASM)

# 仓库里的 dm/ 目录：cpp → main → src → entry → app → ohos → dm，共上 6 层
set(DM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../")

add_library(dmprobe SHARED
    dm_probe.c
    vk_probe.c
    napi_init.c
    # 阶段 1 的 Vulkan 冒烟后端（离屏：清屏 + 读回逐像素验证；对标 dm/gfx/metal/mtl_backend）
    ${DM_DIR}/ohos/vk_backend.c
    # 阶段 3 的共享 Vulkan 上下文 + 客体堆预留（零拷贝的地基；见 vk_ctx_ohos.h 顶部的理由）
    ${DM_DIR}/ohos/vk_ctx_ohos.c
    # 阶段 3 第一步的池模块：鸿蒙侧回放层的第一个文件（对标 nvn_mtl.m 的池那部分）
    ${DM_DIR}/gfx/nvn/nvn_vk.c
    # 宿主单测跑测框架 + 统一日志（框架靠 dlopen 去载入各单测 .so，所以它自己很薄）
    ${DM_DIR}/ohos/host_tests.c
    ${DM_DIR}/ohos/host_tests_gen.c   # 生成：单测清单（哪些 .so、叫什么名字）
    ${DM_DIR}/ohos/dm_log.c
    # 被测对象：鸿蒙版 map_image（与宿主 loader.c 用的是同一份）
    ${DM_DIR}/ohos/map_image_ohos.c
    # 测试映像：由 dm/ohos/tools/gen_embed_ohos.py --embed-bytes 生成，
    # 里面是自定义段 dmmainx0 / dmmainw / dmmainx1（实验 D 用）
    test_image.S
)

target_include_directories(dmprobe PRIVATE
    ${DM_DIR}/host     # dm.h（module_t）
    ${DM_DIR}/ohos     # map_image_ohos.h、host_tests.h、dm_log.h、vk_ctx_ohos.h
    ${DM_DIR}/gfx/nvn  # nvn_vk.h（鸿蒙侧回放层的池模块）
)

# 官方要求：用 NAPI 要链 libace_napi.z.so；打 hilog 要链 libhilog_ndk.z.so；
# 图形层能力探测要链 libvulkan.so（NDK sysroot 里就有）。
target_link_libraries(dmprobe PUBLIC libace_napi.z.so libhilog_ndk.z.so libvulkan.so)

# ---- 宿主单测：每个单测一个 .so（由 tools/gen_host_tests.py 从上游 Makefile 生成）----
# 结构照上游"每个单测一个可执行文件"。跑测框架按名字 dlopen 它们、取各自的 main 来调。
# 这一段是生成文件，改单测清单请改 dm/host/tests/Makefile 再重跑生成器。
include(${DM_DIR}/ohos/host_tests_gen.cmake)
