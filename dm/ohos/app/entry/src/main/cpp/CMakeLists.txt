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
    # 阶段 4 的出图地基：XComponent 的 OHNativeWindow → VkSurfaceKHR
    ${DM_DIR}/ohos/vk_surface_ohos.c
    # 阶段 3 第一步的池模块：鸿蒙侧回放层的第一个文件（对标 nvn_mtl.m 的池那部分）
    ${DM_DIR}/gfx/nvn/nvn_vk.c
    # 阶段 4（3.4-A/C）贴图：上游那份"给没有 BC 的 GPU 用"的 CPU 解码器 + 块线性解排布
    # （都是平台中立的，直接复用；bcdec 是 dm/third_party 的单头文件）
    ${DM_DIR}/gfx/tex/nvn_bcdec.c
    ${DM_DIR}/gfx/tex/nvn_deswizzle.c
    ${DM_DIR}/gfx/nvn/nvn_tex.c
    # C3：BC → ASTC 4x4 转码（BC 贴图不再解成 RGBA8 上传，省 4~8 倍显存与带宽）
    ${DM_DIR}/gfx/tex/nvn_bc2astc.c
    # C3 第 2 步：贴图上传路径的选路（平台中立）+ 鸿蒙侧的能力查询/格式映射。
    # 为什么分两个文件：选路逻辑与平台无关（苹果侧将来填自己的能力表就能复用），
    # 而"设备认不认某个格式"只有平台知道（Vulkan 查 formatProperties）。
    ${DM_DIR}/gfx/tex/nvn_tex_path.c
    ${DM_DIR}/ohos/dm_tex_ohos.c
    # 宿主单测跑测框架 + 统一日志（框架靠 dlopen 去载入各单测 .so，所以它自己很薄）
    ${DM_DIR}/ohos/host_tests.c
    ${DM_DIR}/ohos/host_tests_gen.c   # 生成：单测清单（哪些 .so、叫什么名字）
    ${DM_DIR}/ohos/dm_log.c
    # 最小 nn::fs：客体 romfs 路径 → 宿主目录（DM_ROMFS，默认 /data/local/tmp/romfs）。
    # 只覆盖 main 导入的 23 个符号；按符号名在重定位时分流（dm_probe.c 的 PLT 循环）。
    ${DM_DIR}/ohos/dm_fs_ohos.c
    # 通用 SDK 胶水层（L1）：vi/oe 等轻量语义按"符号名 → 实现"表组织，与游戏无关。
    ${DM_DIR}/ohos/dm_sdk_ohos.c
    # 通用 libc 胶水层（L1）：memcpy/memset/str*/math/time 按符号名直连宿主。
    # 为什么必须有：这些符号此前全落记录型存根（返回 0）⇒ memcpy 空操作 ⇒ 所有结构体拷贝丢失
    # ⇒ 任务记录全 0 ⇒ RootTask 建不出来（2026-10-19 真机实证）。
    ${DM_DIR}/ohos/dm_libc_ohos.c
    # 通用 svc 同步原语（L1）：事件/仲裁锁/进程级 key/多对象等待。
    # 为什么要有：sdk 自带的 nn::os 站在这些 svc 上；不补这层，M1 切过去第一把锁就挂（2026-10-20）。
    ${DM_DIR}/ohos/dm_svc_sync.c
    # 通用 NVN 胶水层（L1）：设备属性表/原点深度模式/句柄换算等平台中立语义。
    ${DM_DIR}/ohos/dm_nvn_ohos.c
    # 通用 NVN 内存池/缓冲胶水层（L1，UMA 直通；从探针搬出）。
    ${DM_DIR}/ohos/dm_nvn_vk.c
    # romfs 启动子集的内嵌归档（段 dmromfs，.incbin 一份 .pack；打档工具 mk_romfs_pack.py）。
    # 为什么内嵌：鸿蒙沙箱看不到 /data/local/tmp，hdc 也写不进应用沙箱 ⇒ 只能放进 HAP。
    ${DM_DIR}/ohos/dm_romfs.S
    # 被测对象：鸿蒙版 map_image（与宿主 loader.c 用的是同一份）
    ${DM_DIR}/ohos/map_image_ohos.c
    # 门机制三件套（实验 H 门回路）：分派器/登记（hle.c）、svc 语义（hle_svc.c）、
    # 门公共入口 dm_gate（entry.S，汇编：存 x0..x30+向量寄存器 → gate_dispatch → 回写）
    ${DM_DIR}/host/hle.c
    ${DM_DIR}/host/hle_svc.c
    ${DM_DIR}/host/entry.S
    # 测试映像：由 dm/ohos/tools/gen_embed_ohos.py --embed-bytes 生成，
    # 里面是自定义段 dmmainx0 / dmmainw / dmmainx1（实验 D 用）
    test_image.S
    # 真实模块：旷野之息 main 的改写产物（同款工具，默认 .incbin 引 F:/switch-work 的
    # main.patched.bin；实验 G 装载验证用。换游戏/换代后重新生成）
    ${DM_DIR}/ohos/dm_images.S
    # 洞内插桩桩：由探针整段复制进映像的代码洞（见 hook_stub.S 抬头注释）
    hook_stub.S
)

target_include_directories(dmprobe PRIVATE
    ${DM_DIR}/host            # dm.h（module_t）
    ${DM_DIR}/ohos            # map_image_ohos.h、host_tests.h、dm_log.h、vk_ctx_ohos.h
    ${DM_DIR}/gfx/nvn         # nvn_vk.h、nvn_obj.h（命令记录格式）
    ${DM_DIR}/gfx/nvn/include # nvn.h（NVN ABI 类型）
)

# 官方要求：用 NAPI 要链 libace_napi.z.so；打 hilog 要链 libhilog_ndk.z.so；
# 图形层能力探测要链 libvulkan.so（NDK sysroot 里就有）。
# XComponent（出图那一步要拿 OHNativeWindow）在 libace_ndk.z.so 里。
# libnative_display_soloist.so 是 3.3-B 的报时员（OH_DisplaySoloist，头文件 native_display_soloist/）：
# 系统每拍 VSync 调一次回调，用来量真实帧间隔、设期望帧率。
target_link_libraries(dmprobe PUBLIC libace_napi.z.so libace_ndk.z.so libhilog_ndk.z.so libvulkan.so
    libnative_display_soloist.so m)   # m：3.4-E 用 log10 算 PSNR（bionic 里数学函数在独立 libm 里）

# ---- 宿主单测：每个单测一个 .so（由 tools/gen_host_tests.py 从上游 Makefile 生成）----
# 结构照上游"每个单测一个可执行文件"。跑测框架按名字 dlopen 它们、取各自的 main 来调。
# 这一段是生成文件，改单测清单请改 dm/host/tests/Makefile 再重跑生成器。
include(${DM_DIR}/ohos/host_tests_gen.cmake)

# ---- 图形层的贴图单测（3.4-B）：上游 dm/gfx/tex/tests/t_tex.c ----
# 形状与宿主单测完全一样（一个单测一个 .so，由探针按名字 dlopen 后取 main 调）——
# 为什么不自己另写一套判据：上游那个单测里 A 往返 / B 手算地址 / C 真数据对拍 / D 错块高负对照 /
# E 手工 3D 标记 五套判据都是现成的，而且 B 的期望值连推导过程都写在注释里（手算），照抄不如收编。
# 块线性那套代码（nvn_tex.c / nvn_deswizzle.c）**平台中立**，直接编进来，零改动。
add_library(dmt_tex_layout SHARED
    "${DM_REPO}/dm/gfx/tex/tests/t_tex.c"
    "${DM_REPO}/dm/gfx/tex/nvn_deswizzle.c"
    "${DM_REPO}/dm/gfx/nvn/nvn_tex.c"
)
dm_host_test_config(dmt_tex_layout)

# ---- 图形层的节拍单测（并入上游时新带进来的 t_pace）：nvn_pace / nvn_pace_hz ----
# 纯 C、只依赖 stdio 与 nvn_pace.h（上游的构建规则就是 `clang -O1 -Wall -o t_pace t_pace.c`），
# 所以照同一形状收编。期望值都是上游**手算**的，正好给"出图帧率该取哪一档"一个现成判据。
add_library(dmt_t_pace SHARED
    "${DM_REPO}/dm/gfx/tests/t_pace.c"
)
dm_host_test_config(dmt_t_pace)
