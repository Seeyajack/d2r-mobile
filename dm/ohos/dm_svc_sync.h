// Horizon svc 同步原语（L1 通用）—— 实现见 dm_svc_sync.c 顶部注释。
// Horizon svc synchronisation primitives (L1, generic); see the .c for the rationale.
//
// 覆盖：0x11 SignalEvent / 0x12 ClearEvent / 0x17 ResetSignal /
//       0x18 WaitSynchronization / 0x19 CancelSynchronization /
//       0x1A ArbitrateLock / 0x1B ArbitrateUnlock /
//       0x1C WaitProcessWideKeyAtomic / 0x1D SignalProcessWideKey
//
// 现状（2026-10-20 np150 更正）：M1 切到 sdk 自带 nn::os 后**已实际启用**——
//   sdk 的互斥量/事件/条件变量就站在这些 svc 原语上（main 的锁竞争经 ArbitrateLock
//   落到本层）。头注曾写"尚未启用"，那是 import 层 pthread 时代的旧认知，已作废。
// Status: exercised since M1 — the SDK's own nn::os sits on these primitives.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dm_svc_sync_set_log(void (*fn)(const char *, ...));

// 每线程的等待条件变量懒初始化（可重复调用）。
void dm_svc_sync_init(void);

// 分派：返回 1 = 已处理（*ret 即 regs[0] 的返回值），0 = 不归本层管。
int dm_svc_sync_dispatch(int num, uint64_t *regs, uint64_t *ret);

// 当前阻塞者快照（np150）：帧线程 tick 调用；有等待者才打日志。
void dm_svc_sync_wait_report(void);

#ifdef __cplusplus
}
#endif
