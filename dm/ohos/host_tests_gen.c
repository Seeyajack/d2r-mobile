// 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从
// dm/host/tests/Makefile 生成。改单测清单请改 Makefile，再重跑生成器。
// GENERATED -- do not edit by hand. Produced by dm/ohos/tools/gen_host_tests.py from
// dm/host/tests/Makefile; edit the Makefile and re-run the generator instead.
#include "host_tests.h"

// 字段：显示名 / .so 名（不带 lib 前缀与 .so 后缀）/ argv[1] / 环境串
// 环境串里 @ 表示"日志文件所在目录"（真机上是应用数据目录）——上游 recipe 里写的是构建目录。
// 一个单测可以有多条（如 t_i18n 对 7 种语言各跑一次）。
const dm_test_ent dm_host_tests[] = {
    { "t_altstack", "dmt_t_altstack", "", "" },
    { "t_audio", "dmt_t_audio", "", "" },
    { "t_audio_spsc", "dmt_t_audio_spsc", "", "" },
    { "t_cloudsync", "dmt_t_cloudsync", "", "" },
    { "t_cloudsync", "dmt_t_cloudsync", "", "TMPDIR=@" },
    { "t_condvar", "dmt_t_condvar", "", "" },
    { "t_doorspot", "dmt_t_doorspot", "", "" },
    { "t_fs", "dmt_t_fs", "", "" },
    { "t_fs_case", "dmt_t_fs_case", "", "" },
    { "t_fs_case", "dmt_t_fs_case", "", "TMPDIR=@" },
    { "t_hid_receipt", "dmt_t_hid_receipt", "", "" },
    { "t_hid_script", "dmt_t_hid_script", "", "" },
    { "t_hud_dead", "dmt_t_hud_dead", "", "" },
    { "t_i18n[en]", "dmt_t_i18n", "en", "TMPDIR=@" },
    { "t_i18n[zh]", "dmt_t_i18n", "zh", "TMPDIR=@" },
    { "t_i18n[none]", "dmt_t_i18n", "none", "TMPDIR=@" },
    { "t_i18n[sysen]", "dmt_t_i18n", "sysen", "TMPDIR=@" },
    { "t_i18n[systw]", "dmt_t_i18n", "systw", "TMPDIR=@" },
    { "t_i18n[tags]", "dmt_t_i18n", "tags", "TMPDIR=@" },
    { "t_i18n[game]", "dmt_t_i18n", "game", "TMPDIR=@" },
    { "t_interaction_diag", "dmt_t_interaction_diag", "", "" },
    { "t_map_diag", "dmt_t_map_diag", "", "" },
    { "t_modetx", "dmt_t_modetx", "", "" },
    { "t_movie", "dmt_t_movie", "", "" },
    { "t_namegen", "dmt_t_namegen", "", "" },
    { "t_oe", "dmt_t_oe", "", "" },
    { "t_padmap", "dmt_t_padmap", "", "" },
    { "t_rift_wave", "dmt_t_rift_wave", "", "" },
    { "t_romfs_validate", "dmt_t_romfs_validate", "", "" },
    { "t_romfs_validate", "dmt_t_romfs_validate", "", "DM_ROMFS=$(DM_ROOT)/data/switch/3.3.94012/Update/RomFS" },
    { "t_rwlv", "dmt_t_rwlv", "", "" },
    { "t_swkbd", "dmt_t_swkbd", "", "" },
    { "t_target_rank", "dmt_t_target_rank", "", "" },
    { "t_textbox", "dmt_t_textbox", "", "" },
    { "t_thread_leak", "dmt_t_thread_leak", "", "" },
    { "t_touch", "dmt_t_touch", "", "" },
    { "t_touch_mac", "dmt_t_touch_mac", "", "" },
    { "t_unit_level", "dmt_t_unit_level", "", "" },
    { "t_warp_diag", "dmt_t_warp_diag", "", "" },
    { "t_widget", "dmt_t_widget", "", "" },
};
const int dm_host_tests_count = (int)(sizeof dm_host_tests / sizeof dm_host_tests[0]);
