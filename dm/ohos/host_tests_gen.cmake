# 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从
# dm/host/tests/Makefile 生成。改单测请改 Makefile，再重跑生成器。
# GENERATED -- do not edit by hand.
#
# 结构照上游"每个单测一个可执行文件"——在鸿蒙侧对应"每个单测一个 .so"。
# 合成一个会得到大量未解析符号（ELF 的 -shared 导出全部符号，丢不掉没用的），见生成器注释。
# 用到之前请先定义 DM_DIR = 仓库里的 dm/ 目录（与 app 的 CMakeLists 约定一致）；
# 本片段自己推出仓库根 DM_REPO = dm/ 的上一层，再用它拼所有路径。
get_filename_component(DM_REPO "${DM_DIR}/.." ABSOLUTE)

# 单测自己的 include 目录
set(DM_HOST_TEST_INCS
    "${DM_REPO}/dm/host"
    "${DM_REPO}/dm/ios/Sources"
)

# 单测需要的宏（上游 Makefile 里那些 -D）
set(DM_HOST_TEST_DEFS DM_TEST_HOOKS)

# 每个单测 .so 都要带的支撑源：垫片层 + 我们自己的日志/音频
set(DM_HOST_TEST_SUPPORT
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)

# 单测 .so 清单（与 host_tests_gen.c 里的 lib 名一致）
set(DM_HOST_TEST_LIBS
    dmt_t_altstack
    dmt_t_audio
    dmt_t_audio_spsc
    dmt_t_cloudsync
    dmt_t_condvar
    dmt_t_doorspot
    dmt_t_fs
    dmt_t_fs_case
    dmt_t_hid_receipt
    dmt_t_hid_script
    dmt_t_hud_dead
    dmt_t_i18n
    dmt_t_modetx
    dmt_t_movie
    dmt_t_namegen
    dmt_t_oe
    dmt_t_padmap
    dmt_t_romfs_validate
    dmt_t_rwlv
    dmt_t_swkbd
    dmt_t_target_rank
    dmt_t_textbox
    dmt_t_thread_leak
    dmt_t_touch
    dmt_t_touch_mac
    dmt_t_unit_level
    dmt_t_widget
)

# 一段辅助宏：给一个单测 .so 配好 include / 宏 / 链接（**用 macro 不用 function**）。
# 为什么用 macro：macro 在调用者的作用域里展开，读 DM_HOST_TEST_SUPPORT 这类变量不会遇到
# 函数作用域带来的意外；这里宁可直白，也不要那种"看着对、展开后不对"的写法。
macro(dm_host_test_config name)
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")
    # BEFORE + 垫片目录在最前：`#include <TargetConditionals.h>` 之类才会命中替身
    target_include_directories(${name} BEFORE PRIVATE
        "${DM_REPO}/dm/ohos/compat" "${DM_REPO}/dm/ohos" ${DM_HOST_TEST_INCS})
    target_compile_definitions(${name} PRIVATE
        __OHOS__=1 DM_LOG_TO_HILOG=1 _GNU_SOURCE=1 ${DM_HOST_TEST_DEFS})
    target_compile_options(${name} PRIVATE -include shim_prefix.h -w)
    # 只链系统库：每个单测 .so 自成一体，跑测框架按需 dlopen
    target_link_libraries(${name} PUBLIC libhilog_ndk.z.so m)
endmacro()

# ---- t_altstack ----
add_library(dmt_t_altstack SHARED
    "${DM_REPO}/dm/host/tests/t_altstack.c"
    "${DM_REPO}/dm/host/tests/t_altstack_stub.c"
    "${DM_REPO}/dm/host/crash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_altstack)

# ---- t_audio ----
add_library(dmt_t_audio SHARED
    "${DM_REPO}/dm/host/tests/t_audio.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_audio)

# ---- t_audio_spsc ----
add_library(dmt_t_audio_spsc SHARED
    "${DM_REPO}/dm/host/tests/t_audio_spsc.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_audio_spsc)

# ---- t_cloudsync ----
add_library(dmt_t_cloudsync SHARED
    "${DM_REPO}/dm/host/tests/t_cloudsync.c"
    "${DM_REPO}/dm/host/dm_cloudsync.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_cloudsync)

# ---- t_condvar ----
add_library(dmt_t_condvar SHARED
    "${DM_REPO}/dm/host/tests/t_condvar.c"
    "${DM_REPO}/dm/host/tests/t_condvar_stub.c"
    "${DM_REPO}/dm/host/hle_thread.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_condvar)

# ---- t_doorspot ----
add_library(dmt_t_doorspot SHARED
    "${DM_REPO}/dm/host/tests/t_doorspot.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_doorspot)

# ---- t_fs ----
add_library(dmt_t_fs SHARED
    "${DM_REPO}/dm/host/tests/t_fs.c"
    "${DM_REPO}/dm/host/tests/t_fs_stub.c"
    "${DM_REPO}/dm/host/dm_i18n.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_fs)

# ---- t_fs_case ----
add_library(dmt_t_fs_case SHARED
    "${DM_REPO}/dm/host/tests/t_fs_case.c"
    "${DM_REPO}/dm/host/dm_i18n.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_fs_case)

# ---- t_hid_receipt ----
add_library(dmt_t_hid_receipt SHARED
    "${DM_REPO}/dm/host/tests/t_hid_receipt.c"
    "${DM_REPO}/dm/host/tests/t_touch_tick_stub.c"
    "${DM_REPO}/dm/host/dm_cursor.c"
    "${DM_REPO}/dm/host/dm_hud.c"
    "${DM_REPO}/dm/host/touch_mac.c"
    "${DM_REPO}/dm/ios/Sources/dm_touch_controls.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_hid_receipt)

# ---- t_hid_script ----
add_library(dmt_t_hid_script SHARED
    "${DM_REPO}/dm/host/tests/t_hid_script.c"
    "${DM_REPO}/dm/host/tests/t_touch_tick_stub.c"
    "${DM_REPO}/dm/host/dm_cursor.c"
    "${DM_REPO}/dm/host/dm_hud.c"
    "${DM_REPO}/dm/host/touch_mac.c"
    "${DM_REPO}/dm/ios/Sources/dm_touch_controls.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_hid_script)

# ---- t_hud_dead ----
add_library(dmt_t_hud_dead SHARED
    "${DM_REPO}/dm/host/tests/t_hud_dead.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_hud_dead)

# ---- t_i18n ----
add_library(dmt_t_i18n SHARED
    "${DM_REPO}/dm/host/tests/t_i18n.c"
    "${DM_REPO}/dm/host/dm_i18n.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_i18n)

# ---- t_modetx ----
add_library(dmt_t_modetx SHARED
    "${DM_REPO}/dm/host/tests/t_modetx.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_modetx)

# ---- t_movie ----
add_library(dmt_t_movie SHARED
    "${DM_REPO}/dm/host/tests/t_movie.c"
    "${DM_REPO}/dm/host/hle_movie.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_movie)

# ---- t_namegen ----
add_library(dmt_t_namegen SHARED
    "${DM_REPO}/dm/host/tests/t_namegen.c"
    "${DM_REPO}/dm/host/dm_namegen.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_namegen)

# ---- t_oe ----
add_library(dmt_t_oe SHARED
    "${DM_REPO}/dm/host/tests/t_oe.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_oe)

# ---- t_padmap ----
add_library(dmt_t_padmap SHARED
    "${DM_REPO}/dm/host/tests/t_padmap.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_padmap)

# ---- t_romfs_validate ----
add_library(dmt_t_romfs_validate SHARED
    "${DM_REPO}/dm/host/tests/t_romfs_validate.c"
    "${DM_REPO}/dm/host/romfs_validate.c"
    "${DM_REPO}/dm/host/dm_i18n.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_romfs_validate)

# ---- t_rwlv ----
add_library(dmt_t_rwlv SHARED
    "${DM_REPO}/dm/host/tests/t_rwlv.c"
    "${DM_REPO}/dm/host/dm_rwlv_core.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_rwlv)

# ---- t_swkbd ----
add_library(dmt_t_swkbd SHARED
    "${DM_REPO}/dm/host/tests/t_swkbd.c"
    "${DM_REPO}/dm/host/hle_swkbd.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_swkbd)

# ---- t_target_rank ----
add_library(dmt_t_target_rank SHARED
    "${DM_REPO}/dm/host/tests/t_target_rank.c"
    "${DM_REPO}/dm/host/hle_target.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_target_rank)

# ---- t_textbox ----
add_library(dmt_t_textbox SHARED
    "${DM_REPO}/dm/host/tests/t_textbox.c"
    "${DM_REPO}/dm/host/dm_textbox_core.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_textbox)

# ---- t_thread_leak ----
add_library(dmt_t_thread_leak SHARED
    "${DM_REPO}/dm/host/tests/t_thread_leak.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_thread_leak)

# ---- t_touch ----
add_library(dmt_t_touch SHARED
    "${DM_REPO}/dm/host/tests/t_touch.c"
    "${DM_REPO}/dm/host/hle_touch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_touch)

# ---- t_touch_mac ----
add_library(dmt_t_touch_mac SHARED
    "${DM_REPO}/dm/host/tests/t_touch_mac.c"
    "${DM_REPO}/dm/host/dm_cursor.c"
    "${DM_REPO}/dm/host/dm_hud.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_touch_mac)

# ---- t_unit_level ----
add_library(dmt_t_unit_level SHARED
    "${DM_REPO}/dm/host/tests/t_unit_level.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_unit_level)

# ---- t_widget ----
add_library(dmt_t_widget SHARED
    "${DM_REPO}/dm/host/tests/t_widget.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_dispatch.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_fs.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_hash.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_mach.c"
    "${DM_REPO}/dm/ohos/compat/src/dm_compat_str.c"
    "${DM_REPO}/dm/ohos/dm_log.c"
    "${DM_REPO}/dm/ohos/audio_ohos.c"
)
dm_host_test_config(dmt_t_widget)

