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

// 回放（M4 真执行）：按记录顺序回调。emit 返回非 0 表示提前停（要停在那条命令之前）。
// 执行层据此把命令翻译成 Vulkan 调用，不用知道 cb_op 的结构。
int dm_nvn_cb_replay(void *cb, int (*emit)(int id, const char *name,
                                           uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, void *ud),
                     void *ud);

// 名字（去掉 nvnCommandBuffer 前缀）→ 下标；找不到返回 -1
int dm_nvn_cb_index(const char *short_name);

