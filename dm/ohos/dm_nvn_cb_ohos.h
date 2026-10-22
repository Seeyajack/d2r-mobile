// dm_nvn_cb_ohos.h —— NVN 命令缓冲「记录层」骨架（M4-c 第二步，2026-10-22）
//
// 定位（别误读）：这是**记录层**，不是执行层。
//   · 记录：127 个 CommandBuffer 函数全部有实现——每个把"谁被调了 + 参数"追加进该 CB 的命令流；
//   · 执行：Queue::SubmitCommands 目前只做**统计摘要**（哪些命令被记了多少条），
//     真正的 Vulkan 回放等真机（要 VkCommandBuffer 与状态缓存配合）。
// 为什么要先做记录层：127 个函数里只有 ~20 个是"真执行"（Draw/Clear/Copy/Dispatch），
//   其余 107 个是**状态设置/绑定**。把记录做对，出帧时才知道"游戏到底按什么顺序摆状态"。
#pragma once
#include <stdint.h>

void *dm_nvn_cb_lookup(const char *name);
int   dm_nvn_cb_count(void);
int   dm_nvn_cb_selftest(void);

// Queue::SubmitCommands 调它：把一个 CB 记下的命令流汇总成一个摘要（每条命令几次）
void dm_nvn_cb_summary(void *cb, char *out, int cap);

