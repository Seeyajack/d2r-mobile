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
    napi_init.c
    # 被测对象：鸿蒙版 map_image（与宿主 loader.c 用的是同一份）
    ${DM_DIR}/ohos/map_image_ohos.c
    # 测试映像：由 dm/ohos/tools/gen_embed_ohos.py --embed-bytes 生成，
    # 里面是自定义段 dmmainx0 / dmmainw / dmmainx1（实验 D 用）
    test_image.S
)

target_include_directories(dmprobe PRIVATE
    ${DM_DIR}/host     # dm.h（module_t）
    ${DM_DIR}/ohos     # map_image_ohos.h
)

# 官方要求：用 NAPI 要链 libace_napi.z.so；打 hilog 要链 libhilog_ndk.z.so
target_link_libraries(dmprobe PUBLIC libace_napi.z.so libhilog_ndk.z.so)
