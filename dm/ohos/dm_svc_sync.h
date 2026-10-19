// Horizon svc 同步原语（L1 通用）—— 实现见 dm_svc_sync.c 顶部注释。
// Horizon svc synchronisation primitives (L1, generic); see the .c for the rationale.
//
// 覆盖：0x11 SignalEvent / 0x12 ClearEvent / 0x17 ResetSignal /
//       0x18 WaitSynchronization / 0x19 CancelSynchronization /
//       0x1A ArbitrateLock / 0x1B ArbitrateUnlock /
//       0x1C WaitProcessWideKeyAtomic / 0x1D SignalProcessWideKey
//
// 现状：**已接线但尚未启用**。main 的 nn::os 仍走宿主实现（导入层 pthread），
//   所以这些 svc 号目前没人调 ⇒ 接线本身零风险。等切到 sdk 自带的 nn::os 时才会用到。
// Status: wired but not yet exercised. main's nn::os still uses the host implementation, so these
// svc numbers are unused for now; they come into play once we switch to the SDK's own nn::os.
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

#ifdef __cplusplus
}
#endif
