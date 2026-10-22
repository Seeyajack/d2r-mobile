// dm_nvn_state_ohos.h —— NVN 状态族（纯数据）的宿主实现接口（M4-a，2026-10-22）
//
// 覆盖 8 个族共 77 个函数：Blend / ChannelMask / Color / DepthStencil / Multisample /
// Polygon / VertexAttrib / VertexStream。
// 设计要点：值存在"指针 → 影子字段"的旁表里，**不往游戏给的对象里写**（避免越界）；
// Get 找不到时返回该族的 NVN 规范默认值。
#pragma once
#include <stdint.h>

// 按名字取函数（没有则返回 NULL）；由 dm_nvn_bootstrap() 在正式表之后调用。
void *dm_nvn_state_lookup(const char *name);

// 表里注册的函数个数（判据用：应为 77）
int dm_nvn_state_count(void);

// 自测：逐族 Set→Get 往返 + 未知对象回落默认值；返回失败项数（0 = 全过）。
// 只验"存进去能读回来"，不验语义正确性（那要靠出帧比对）。
int dm_nvn_state_selftest(void);
