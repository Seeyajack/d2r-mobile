# dm/ohos/compat —— 苹果头文件的鸿蒙替身（垫片层）

> 建立：2026-10-13 深夜 ｜ 依据：对 `dm/host/` 全部 75 个源文件的头文件普查 + 符号普查（不是推测）
> 实测：`bash ../tools/probe_host.sh` —— **71 个 .c 里 62 个已能单独编译通过**（基线是 0）

---

## 一、这一层是干什么的

宿主（`dm/host/`）当初是给 macOS / iOS 写的，里面对系统能力的调用分两种：

| 类型 | 例子 | 移植时怎么办 |
|---|---|---|
| **标准 C / POSIX**（占绝大多数） | `string.h`、`pthread.h`、`unistd.h`、`sys/mman.h` | ✅ 鸿蒙 NDK 里都有，一个字不用改 |
| **苹果独有**（少数，但卡在关键路径上） | `TargetConditionals.h`、`mach/*`、`CommonCrypto/*`、`dispatch/*` | ⚠️ 要有替身 |

这一层就是**给第二种东西做替身**。

**关键手法（能做到"上游零改动"的原因）**：宿主的苹果头文件全部是尖括号包含：

```c
#include <mach/mach.h>
```

于是只要在同名路径下放一个自己的头（`compat/mach/mach.h`），再让编译器**先**搜本目录
（构建脚本里的 `-I dm/ohos/compat` 排在 sysroot 之前），`#include` 就会命中替身。
**上游文件一个字都不用改**——这一点很重要，意味着将来 `git fetch upstream` 不会因为这些改动打架。

---

## 二、里面有什么（文件清单）

| 垫片 | 顶替什么 | 做法 | 性质 |
|---|---|---|---|
| `TargetConditionals.h` | 平台判断宏 | `TARGET_OS_IPHONE` 等一律 0 | 真实现 |
| `mach/mach_time.h` | `mach_absolute_time` | `clock_gettime(CLOCK_MONOTONIC)`，**数值语义与苹果一致（1/1 时基、纳秒）** | 真实现 |
| `mach/mach.h` | Mach 内核接口 | 见下节，分三种性质 | 混合 |
| `libkern/OSCacheControl.h` | `sys_icache_invalidate` | `__builtin___clear_cache()` | 真实现 |
| `pthread/qos.h` | 线程服务质量等级 | 类型齐全、调用no-op（**不假装改了优先级**） | 诚实降级 |
| `CommonCrypto/CommonDigest.h` | MD5 / SHA-1 / SHA-256 | 自带三份标准实现，无第三方依赖 | 真实现 |
| `sys/sysctl.h` | `KERN_PROC_PID` 取进程启动时刻 | 由 `/proc/self/stat` 第 22 字段 + `/proc/uptime` 反推 | 真实现 |
| `sys/clonefile.h` | APFS 写时复制克隆 | 递归拷贝（功能等价、代价更高） | 功能等价 |
| `sys/xattr.h` | 扩展属性（**6 参数**版） | 映射到 Linux 的 4/5 参数接口 | 真实现 |
| `malloc/malloc.h` | malloc zone 统计 | 填 0 + 明确说明 | 诚实降级 |
| `shim_prefix.h` | 一批散用符号 | `strlcpy`/`strlcat`/`arc4random*`/`renamex_np`/`st_mtimespec`/`CLOCK_UPTIME_RAW` | 真实现 |

实现代码在 `src/`，分四个文件：`dm_compat_str.c`、`dm_compat_hash.c`、`dm_compat_mach.c`、`dm_compat_fs.c`。

---

## 三、`mach/mach.h` 的三类东西（最需要说清的一块）

宿主对 Mach 的调用**性质完全不同**，混在一起说会误导，所以分开：

### ① 取时钟 —— 真实现
`mach_absolute_time()` 在苹果 arm64 上就是纳秒（时基 1/1），这里给同样的数值语义，
上游 `mach_absolute_time() * tb.numer / tb.denom` 不用改、结果也不变。

### ② 安全读内存 —— 真实现
`vm_read_overwrite(mach_task_self(), addr, n, buf, &got)`，宿主用了 **26 处**
（diag.c / dm_bright.c / hle_target.c / hle_private_aim.c / hle_thread.c）。
它要的是"读一个**可能非法**的地址，读不到就返回失败、不要崩"。

鸿蒙上的等价物是 **`/proc/self/mem` + `pread`**：地址非法时 `pread` 返回 `-1`/`EFAULT`，进程不崩。
退路是 `process_vm_readv`。**两条都不可用时返回失败，绝不退回 `memcpy`**——
`memcpy` 遇到非法地址直接段错误，那是宿主最不能接受的结果。宁可少读一段诊断内存。

### ③ 别的线程的寄存器/状态 —— 诚实失败（**不假装成功**）
`thread_get_state` / `thread_suspend` / `task_threads` 只在**一个地方**用到：
`hle_thread.c` 的 `thr_bt()`——看门狗 / 崩溃时导出**别的客体线程**的寄存器和栈。
Linux/鸿蒙没有"非特权读另一个线程寄存器"的接口，`ptrace` 要权限、要挂起目标、还会改变目标状态，
在游戏进程里做这件事风险和收益完全不成比例。

所以一律返回 `KERN_FAILURE` ⇒ `thr_bt()` 在第一步就返回，看门狗日志里**少一段别的线程的现场**，
**游戏行为一行不受影响**。日志里会打一句说明，不会让人以为是"本来就没有"。

同一原则也用在 `malloc_zone_statistics`（填 0 不填假数字）和 `pthread/qos.h`（不假装改了优先级）。

---

## 四、实测现状（`tools/probe_host.sh` 可复现）

```
共 71 个 .c，通过 62  未通过 9
```

**未通过的 9 个，分成三类，性质完全不同：**

| 文件 | 卡在哪 | 性质 |
|---|---|---|
| `diag.c` / `dm_bright.c` / `dm_vault_ui.c` | 缺 `dm_hud_exp.h` / `dm_bright_levels.inc` / `dm_vault_tab.h` | **不是移植问题**：这三张表要用**你自己的游戏文件**生成（阶段 2） |
| `hle.c` | `mach-o/getsect.h` | 已知项：改用 `__start_/__stop_` 定位自定义 ELF 段（`loader.c` 已有同样分叉可照抄） |
| `hle_audio.c` | `AudioToolbox` | **音频后端要重写**（CoreAudio → 鸿蒙音频服务），独立一块 |
| `hle_lifecycle.c` | `dispatch/dispatch.h` | GCD + **block 字面量**，需要一并解决 block 运行时 |
| `crash.c` | `mcontext_t` 在两边的定义不同 | 信号上下文结构差异，要按字段改 |
| `hle_fdstat.c` | `F_GETPATH` | 苹果的 fcntl 扩展；等价做法是 `readlink("/proc/self/fd/N")` |
| `dm_textbox_core.c` | 整数赋给指针 | 疑似苹果编址假设，要单独看 |

**这 6 个（去掉 3 个等游戏文件的）就是"把宿主编进 `libdmohos.so`"剩下要攻的全部。**

---

## 五、还没做、也不打算在本层做的

| 缺的东西 | 为什么不做在这儿 |
|---|---|
| `dispatch/dispatch.h` | 涉及 GCD 语义 + block 运行时（`-fblocks` + `Block_copy`），是独立一块工作，草率垫上去会静默出错 |
| `AudioToolbox` | 不是"垫"的事，是**换一个音频后端**（CoreAudio AudioUnit → 鸿蒙音频服务） |
| `CoreFoundation` | `dm_i18n.c` 里已被 `#if TARGET_OS_IPHONE` 守卫排除（垫片的 `TargetConditionals.h` 让它自动落到鸿蒙侧），无需处理 |
| 上游直接写的 `fprintf(stderr, ...)` | `dm.h` 的 `LOG()` 已接 hilog，但少数地方直接写 stderr；真机上 stderr 不一定可见，另案处理 |

---

## 六、待真机验证（**不做"应该没问题"的假定**）

本机没有能运行的 C 编译器，下面几条只能在真机上验：

1. **哈希正确性**：MD5 / SHA-1 / SHA-256 要用标准测试向量在真机上比对一次
   （常量表由程序按定义生成、算法用同构实现对照过 `hashlib`，但 C 版本没跑过机器）。
2. **`/proc/self/mem` 是否可读**：被 seccomp 拦下的话，`vm_read_overwrite` 会全线失败，
   `hle_thread.c` 取线程名、`hle_target.c`、`dm_bright.c` 会静默降级。这是本层**最大的一条待验项**。
3. **`sysctl` 反推的进程启动时刻**是否合理（对着日志里的启动时间线看一眼即可）。
4. **`clonefile` 递归拷贝**在存档目录上的耗时（量级几 MB，预期可接受）。
5. **`renamex_np`**：本机内核支不支持 `renameat2(RENAME_EXCHANGE)`；不支持时会退到非原子三步改名（日志有提示）。
6. **上游的 `LOG()` 长行**（有些接近 1 KiB）在 hilog 里是否被截断。
