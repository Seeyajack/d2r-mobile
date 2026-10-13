// 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从
// dm/host/tests/Makefile 生成。改单测清单请改 Makefile，再重跑生成器。
// GENERATED -- do not edit by hand. Produced by dm/ohos/tools/gen_host_tests.py from
// dm/host/tests/Makefile; edit the Makefile and re-run the generator instead.
#include "host_tests.h"

int dm_test_altstack_main(void);
int dm_test_audio_main(void);
int dm_test_audio_spsc_main(void);
int dm_test_cloudsync_main(void);
int dm_test_condvar_main(void);
int dm_test_doorspot_main(void);
int dm_test_fs_main(void);
int dm_test_fs_case_main(void);
int dm_test_hid_receipt_main(void);
int dm_test_hid_script_main(void);
int dm_test_hud_dead_main(void);
int dm_test_i18n_main(void);
int dm_test_modetx_main(void);
int dm_test_movie_main(void);
int dm_test_namegen_main(void);
int dm_test_oe_main(void);
int dm_test_padmap_main(void);
int dm_test_rift_clock_main(void);
int dm_test_romfs_validate_main(void);
int dm_test_swkbd_main(void);
int dm_test_target_rank_main(void);
int dm_test_textbox_main(void);
int dm_test_thread_leak_main(void);
int dm_test_touch_main(void);
int dm_test_touch_mac_main(void);
int dm_test_unit_level_main(void);
int dm_test_widget_main(void);

const dm_test_ent dm_host_tests[] = {
    { "t_altstack", dm_test_altstack_main },
    { "t_audio", dm_test_audio_main },
    { "t_audio_spsc", dm_test_audio_spsc_main },
    { "t_cloudsync", dm_test_cloudsync_main },
    { "t_condvar", dm_test_condvar_main },
    { "t_doorspot", dm_test_doorspot_main },
    { "t_fs", dm_test_fs_main },
    { "t_fs_case", dm_test_fs_case_main },
    { "t_hid_receipt", dm_test_hid_receipt_main },
    { "t_hid_script", dm_test_hid_script_main },
    { "t_hud_dead", dm_test_hud_dead_main },
    { "t_i18n", dm_test_i18n_main },
    { "t_modetx", dm_test_modetx_main },
    { "t_movie", dm_test_movie_main },
    { "t_namegen", dm_test_namegen_main },
    { "t_oe", dm_test_oe_main },
    { "t_padmap", dm_test_padmap_main },
    { "t_rift_clock", dm_test_rift_clock_main },
    { "t_romfs_validate", dm_test_romfs_validate_main },
    { "t_swkbd", dm_test_swkbd_main },
    { "t_target_rank", dm_test_target_rank_main },
    { "t_textbox", dm_test_textbox_main },
    { "t_thread_leak", dm_test_thread_leak_main },
    { "t_touch", dm_test_touch_main },
    { "t_touch_mac", dm_test_touch_mac_main },
    { "t_unit_level", dm_test_unit_level_main },
    { "t_widget", dm_test_widget_main },
};
const int dm_host_tests_count = (int)(sizeof dm_host_tests / sizeof dm_host_tests[0]);
