// 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从
// dm/host/tests/Makefile 生成。改单测清单请改 Makefile，再重跑生成器。
// GENERATED -- do not edit by hand. Produced by dm/ohos/tools/gen_host_tests.py from
// dm/host/tests/Makefile; edit the Makefile and re-run the generator instead.
#include "host_tests.h"

// lib 是不带前缀与后缀的 .so 名（与 cmake 里 OUTPUT_NAME 一致），dlopen 时拼上目录与 .so
const dm_test_ent dm_host_tests[] = {
    { "t_altstack", "dmt_t_altstack" },
    { "t_audio", "dmt_t_audio" },
    { "t_audio_spsc", "dmt_t_audio_spsc" },
    { "t_cloudsync", "dmt_t_cloudsync" },
    { "t_condvar", "dmt_t_condvar" },
    { "t_doorspot", "dmt_t_doorspot" },
    { "t_fs", "dmt_t_fs" },
    { "t_fs_case", "dmt_t_fs_case" },
    { "t_hid_receipt", "dmt_t_hid_receipt" },
    { "t_hid_script", "dmt_t_hid_script" },
    { "t_hud_dead", "dmt_t_hud_dead" },
    { "t_i18n", "dmt_t_i18n" },
    { "t_modetx", "dmt_t_modetx" },
    { "t_movie", "dmt_t_movie" },
    { "t_namegen", "dmt_t_namegen" },
    { "t_oe", "dmt_t_oe" },
    { "t_padmap", "dmt_t_padmap" },
    { "t_romfs_validate", "dmt_t_romfs_validate" },
    { "t_swkbd", "dmt_t_swkbd" },
    { "t_target_rank", "dmt_t_target_rank" },
    { "t_textbox", "dmt_t_textbox" },
    { "t_thread_leak", "dmt_t_thread_leak" },
    { "t_touch", "dmt_t_touch" },
    { "t_touch_mac", "dmt_t_touch_mac" },
    { "t_unit_level", "dmt_t_unit_level" },
    { "t_widget", "dmt_t_widget" },
};
const int dm_host_tests_count = (int)(sizeof dm_host_tests / sizeof dm_host_tests[0]);
