# Horizon svc 同步原语：语义规范与移植计划

> 用途：为 **M1（装载 sdk，让 SDK 自带的 nn::os 跑起来）** 铺路。
> 我们的宿主目前把 `nn::os` 的锁/事件/信号量**在导入层用 pthread 实现**（过渡件）。
> sdk 的 `nn::os` 不是这么做的——它站在 **svc 同步原语**上。不补这一层，切过去第一把锁就挂。

## 0. 参考来源与许可（必读）

本节内容整理自 **[seedds/nxnative](https://github.com/seedds/nxnative)（MIT）** 的
`dm/host/hle_thread.c`，并用 **Atmosphère/mesosphère**（Horizon 内核的公开实现）交叉印证。
nxnative 本身派生自我们上游 **d2r-mobile**（MIT）。

- 本文件是**语义整理**，不是代码副本。
- 若后续直接移植其代码片段，必须：① 在 `NOTICE.md` 的「参考与致谢（同源分支 nxnative）」
  小节逐项登记（文件 / 提交号 / 许可证 / 改动）；② 在源文件头保留原始版权声明。

## 1. svc 编号

| 编号 | 名称 | 作用 |
|---|---|---|
| 0x11 | SignalEvent | 事件置位（**只接受写端句柄**） |
| 0x12 | ClearEvent | 事件清除（读/写端都接受） |
| 0x17 | ResetSignal | 清除读端信号；未置位或句柄是线程 ⇒ `0xFA01` |
| 0x18 | WaitSynchronization | 等一组句柄任一触发；返回触发的**下标** |
| 0x19 | CancelSynchronization | 取消等待（置 cancel 并唤醒） |
| 0x1A | ArbitrateLock | 互斥锁（地址仲裁） |
| 0x1B | ArbitrateUnlock | 解锁（**直接移交**，见 §4） |
| 0x1C | WaitProcessWideKeyAtomic | 条件变量等待（**原子的"放锁+入队"**） |
| 0x1D | SignalProcessWideKey | 条件变量唤醒（count ≤ 0 表示全部） |

错误码：`R_TIMEOUT=0xEA01`、`R_CANCELLED=0xEC01`、`R_BAD_HANDLE=0xE401`、`R_BAD_STATE=0xFA01`。

## 2. 数据结构骨架

```
thr: wkind（0 无 / 1 锁 / 2 条件变量 / 3 地址 / 4 同步对象）
     waddr / wkey / wtag / wseq（注册序号，同优先级 FIFO）/ wres / cancel
     whs / whn（WaitSynchronization 的句柄数组指针与个数）
     cv（**每线程一个**条件变量）
全局：一把大锁 G + 一条等待链 g_wait + 全局序号 g_wseq
事件：{ int signaled; int refs; }  refs=2（读端+写端），两端都 CloseHandle 才释放
句柄表：g_h[4096]，句柄值 0x1000+i，按 type 校验（O_EVENT_R / O_EVENT_W / O_THREAD）
```

**锁字位域**：`lock = owner_handle（低 30 位） | (有等待者 ? 0x40000000 : 0)`
条件变量的 key 是**另一个 32 位字**，`1` = 有人在等/需要唤醒。

## 3. 选取顺序（决定行为是否与真机一致）

数值小的线程优先级优先；同优先级按 `wseq` 先注册先选 —— 与 Horizon 内核的等待树一致。
唤醒后若被唤醒线程与本线程同核且优先级更高，置 `yield_pending`（模拟同核抢占）。

## 4. 四个原语的要点

**ArbitrateLock(addr, owner, tag)**：仅当锁字为 `owner|WAITERS_BIT`（持有者同意移交）时入队等待；
被唤醒即成为 owner。

**ArbitrateUnlock(addr)**：无等待者 ⇒ 锁字清零；有等待者 ⇒ 取队首，锁字写它的 tag
（若还有人则带上 `WAITERS_BIT`），直接唤醒它 —— **是把锁交出去（handoff），不是唤醒后抢**。

**WaitProcessWideKeyAtomic(addr, key, tag, timeout)** —— ⚠ **顺序是硬性要求**：
1. `st32(key, 1)` 置位；
2. `lock_handoff(addr)` 释放互斥量；
3. 入队 `block(t, 2, timeout)`。

反序（先放锁再置 key）会造成："放锁之后、置 key 之前"另一线程改完条件读到 key=0 不唤醒
⇒ 本线程永久等待。nxnative 曾因此**资源表读写锁死锁**，并保留了一个负向对照宏。

**SignalProcessWideKey(key, count)**：按内核顺序取 `wkind==2 && wkey==key` 的等待者；
对每个尝试取锁：空闲则 CAS 拿到并唤醒；被占则 CAS 置 `WAITERS_BIT`、该线程转为锁等待
并**重新编号 wseq**。若之后该 key 已无等待者 ⇒ 清零。

**WaitSynchronization(handles, n, timeout)**：循环「查 cancel → 扫句柄找已触发（返回下标）→
都没触发则以剩余超时入队等待」，被唤醒后重扫。剩余超时 = 截止时刻 − 现在，避免多轮累积误差。
唤醒侧只**定向唤醒**句柄集里确实含该对象的等待者，不要广播。

**超时语义**：`ns < 0` 无限；`ns == 0` 立即 `R_TIMEOUT`；`ns > 0` 定时。
**条件变量超时后不重新获取锁**（由用户态自己获取）。

## 5. 移植到我们的门回路（计划）

1. 新建 `dm/ohos/dm_svc_sync.c`（L1，平台中立）：句柄表 + 事件对象 + 等待链 + block/wake。
2. 在 `hle_svc.c` 的 `svc_dispatch` 增加 0x11/0x12/0x17/0x18/0x19/0x1A/0x1B/0x1C/0x1D 分支。
3. **先不动绑定**：跑一轮确认零回归（这些 svc 目前没人用）。
4. 再按白名单把 `nn::os` 的导入解析到 sdk，逐个开：先 mutex → 再 event → 最后 thread。
5. 判据：手写的 os 钩子调用数降为 0，且游戏推进位置不倒退。

## 6. 与现状的关系（重要）

我们当前在**导入层**用 pthread 实现 nn::os（互斥量表 2048 格、事件侧表、消息队列）。
那套能跑，但和 SDK 自带实现是**两套并行的世界**，每遇到一个新语义就要手补一次。
补完 svc 同步层后，可以逐步摘掉手写实现（保留作兜底，`DM_HOST_OS=1` 再切回）。
