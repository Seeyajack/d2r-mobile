// 【生成文件】由 gen_embed_ohos.py --xmods-header 生成，勿手改。
// 运行时"额外模块"表：除 main/sdk（定制装载）外的全部模块，顺序 = 装载顺序。
// 布局常量来自 dm_main_layout.h（同目录，由 --layout-header 一起生成）。
#pragma once
#include "dm_main_layout.h"

#define DM_XMODS_GEN_N 5
// 字段与 dm_probe.c 的 xmod_t 一一对应（name + module_t 的 8 个布局常数）
static const struct {
    const char *name;
    uint64_t text_end, patch_off, ro_off, data_off, key_page_off, key_page_size, x0_end, img_end;
} dm_xmods_gen[] = {
    { "rtld", DM_IMG_RTLD_TEXT_END, DM_IMG_RTLD_PATCH_OFF, DM_IMG_RTLD_RO_OFF,
      DM_IMG_RTLD_DATA_OFF, DM_IMG_RTLD_KEY_PAGE_OFF, DM_IMG_RTLD_KEY_PAGE_SIZE,
      DM_IMG_RTLD_X0_END, DM_IMG_RTLD_END },
    { "subsdk0", DM_IMG_SUBSDK0_TEXT_END, DM_IMG_SUBSDK0_PATCH_OFF, DM_IMG_SUBSDK0_RO_OFF,
      DM_IMG_SUBSDK0_DATA_OFF, DM_IMG_SUBSDK0_KEY_PAGE_OFF, DM_IMG_SUBSDK0_KEY_PAGE_SIZE,
      DM_IMG_SUBSDK0_X0_END, DM_IMG_SUBSDK0_END },
    { "subsdk1", DM_IMG_SUBSDK1_TEXT_END, DM_IMG_SUBSDK1_PATCH_OFF, DM_IMG_SUBSDK1_RO_OFF,
      DM_IMG_SUBSDK1_DATA_OFF, DM_IMG_SUBSDK1_KEY_PAGE_OFF, DM_IMG_SUBSDK1_KEY_PAGE_SIZE,
      DM_IMG_SUBSDK1_X0_END, DM_IMG_SUBSDK1_END },
    { "subsdk2", DM_IMG_SUBSDK2_TEXT_END, DM_IMG_SUBSDK2_PATCH_OFF, DM_IMG_SUBSDK2_RO_OFF,
      DM_IMG_SUBSDK2_DATA_OFF, DM_IMG_SUBSDK2_KEY_PAGE_OFF, DM_IMG_SUBSDK2_KEY_PAGE_SIZE,
      DM_IMG_SUBSDK2_X0_END, DM_IMG_SUBSDK2_END },
    { "subsdk3", DM_IMG_SUBSDK3_TEXT_END, DM_IMG_SUBSDK3_PATCH_OFF, DM_IMG_SUBSDK3_RO_OFF,
      DM_IMG_SUBSDK3_DATA_OFF, DM_IMG_SUBSDK3_KEY_PAGE_OFF, DM_IMG_SUBSDK3_KEY_PAGE_SIZE,
      DM_IMG_SUBSDK3_X0_END, DM_IMG_SUBSDK3_END },
};
