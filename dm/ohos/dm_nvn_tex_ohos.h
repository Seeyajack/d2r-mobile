// dm_nvn_tex_ohos.h —— Texture / Sampler / Buffer·MemoryPool·Program·Sync 的宿主实现（M4-a 补完）
// 154 个函数，把游戏请求面补到 100%。策略同状态族：影子旁表（指针 → 32 字段）。
#pragma once
#include <stdint.h>
void *dm_nvn_tex_lookup(const char *name);
int   dm_nvn_tex_count(void);
int   dm_nvn_tex_selftest(void);
