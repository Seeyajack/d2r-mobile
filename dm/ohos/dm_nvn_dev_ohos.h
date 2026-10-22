// dm_nvn_dev_ohos.h —— NVN 设备/窗口/队列的宿主实现（M4-c：首帧前置）
//
// 定位（别误读成"已经能出帧"）：
//   · Builder / Set / Get：真实实现（影子旁表）
//   · Initialize / Finalize：真实记账（日志里能看到游戏建了几个窗口/队列）
//   · AcquireTexture / SubmitCommands / PresentTexture / Finish / Flush / WaitSync：**记账 + 安全桩**
//     （返回成功并计数，不真提交——记录层还没落地）。这样游戏能继续走，我们拿到真实调用序列。
#pragma once
#include <stdint.h>

void *dm_nvn_dev_lookup(const char *name);   // 名字 → 函数（无则 NULL）
int   dm_nvn_dev_count(void);                // 表里函数个数
int   dm_nvn_dev_selftest(void);             // 往返 + 失败语义自测，返回失败项数

// 打点计数器（探针报告里读）
extern uint64_t g_nvn_win_init, g_nvn_win_acquire, g_nvn_q_init, g_nvn_q_submit, g_nvn_q_present,
                g_nvn_q_finish, g_nvn_q_flush, g_nvn_dev_ts;
