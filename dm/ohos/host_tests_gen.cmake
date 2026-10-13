# 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 生成。
# GENERATED -- do not edit by hand.

# 单测源码（相对仓库根）
set(DM_HOST_TEST_SRCS
    "${DM_DIR}/dm/host/tests/t_altstack.c"
    "${DM_DIR}/dm/host/tests/t_altstack_stub.c"
    "${DM_DIR}/dm/host/crash.c"
    "${DM_DIR}/dm/host/tests/t_audio.c"
    "${DM_DIR}/dm/host/tests/t_audio_spsc.c"
    "${DM_DIR}/dm/host/tests/t_cloudsync.c"
    "${DM_DIR}/dm/host/dm_cloudsync.c"
    "${DM_DIR}/dm/host/tests/t_condvar.c"
    "${DM_DIR}/dm/host/tests/t_condvar_stub.c"
    "${DM_DIR}/dm/host/hle_thread.c"
    "${DM_DIR}/dm/host/tests/t_doorspot.c"
    "${DM_DIR}/dm/host/tests/t_fs.c"
    "${DM_DIR}/dm/host/tests/t_fs_stub.c"
    "${DM_DIR}/dm/host/dm_i18n.c"
    "${DM_DIR}/dm/host/tests/t_fs_case.c"
    "${DM_DIR}/dm/host/tests/t_hid_receipt.c"
    "${DM_DIR}/dm/host/tests/t_touch_tick_stub.c"
    "${DM_DIR}/dm/host/dm_cursor.c"
    "${DM_DIR}/dm/host/dm_hud.c"
    "${DM_DIR}/dm/host/touch_mac.c"
    "${DM_DIR}/dm/ios/Sources/dm_touch_controls.c"
    "${DM_DIR}/dm/host/tests/t_hid_script.c"
    "${DM_DIR}/dm/host/tests/t_hud_dead.c"
    "${DM_DIR}/dm/host/tests/t_i18n.c"
    "${DM_DIR}/dm/host/tests/t_modetx.c"
    "${DM_DIR}/dm/host/tests/t_movie.c"
    "${DM_DIR}/dm/host/hle_movie.c"
    "${DM_DIR}/dm/host/tests/t_namegen.c"
    "${DM_DIR}/dm/host/dm_namegen.c"
    "${DM_DIR}/dm/host/tests/t_oe.c"
    "${DM_DIR}/dm/host/tests/t_padmap.c"
    "${DM_DIR}/dm/host/tests/t_rift_clock.c"
    "${DM_DIR}/dm/host/hle_os.c"
    "${DM_DIR}/dm/host/tests/t_romfs_validate.c"
    "${DM_DIR}/dm/host/romfs_validate.c"
    "${DM_DIR}/dm/host/tests/t_swkbd.c"
    "${DM_DIR}/dm/host/hle_swkbd.c"
    "${DM_DIR}/dm/host/tests/t_target_rank.c"
    "${DM_DIR}/dm/host/hle_target.c"
    "${DM_DIR}/dm/host/tests/t_textbox.c"
    "${DM_DIR}/dm/host/dm_textbox_core.c"
    "${DM_DIR}/dm/host/tests/t_thread_leak.c"
    "${DM_DIR}/dm/host/tests/t_touch.c"
    "${DM_DIR}/dm/host/hle_touch.c"
    "${DM_DIR}/dm/host/tests/t_touch_mac.c"
    "${DM_DIR}/dm/host/tests/t_unit_level.c"
    "${DM_DIR}/dm/host/tests/t_widget.c"
)

# 单测自己的 include 目录（相对仓库根）
set(DM_HOST_TEST_INCS
    "${DM_DIR}/dm/host"
    "${DM_DIR}/dm/ios/Sources"
)

# 单测需要的宏
set(DM_HOST_TEST_DEFS DM_TEST_HOOKS)

# 把每个单测的 main 改名，避免 27 个 main 撞车
set_source_files_properties("${DM_DIR}/dm/host/tests/t_altstack.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_altstack_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_audio.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_audio_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_audio_spsc.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_audio_spsc_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_cloudsync.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_cloudsync_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_condvar.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_condvar_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_doorspot.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_doorspot_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_fs.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_fs_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_fs_case.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_fs_case_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_hid_receipt.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_hid_receipt_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_hid_script.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_hid_script_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_hud_dead.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_hud_dead_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_i18n.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_i18n_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_modetx.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_modetx_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_movie.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_movie_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_namegen.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_namegen_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_oe.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_oe_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_padmap.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_padmap_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_rift_clock.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_rift_clock_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_romfs_validate.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_romfs_validate_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_swkbd.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_swkbd_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_target_rank.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_target_rank_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_textbox.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_textbox_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_thread_leak.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_thread_leak_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_touch.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_touch_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_touch_mac.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_touch_mac_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_unit_level.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_unit_level_main")
set_source_files_properties("${DM_DIR}/dm/host/tests/t_widget.c" PROPERTIES COMPILE_DEFINITIONS "main=dm_test_widget_main")
