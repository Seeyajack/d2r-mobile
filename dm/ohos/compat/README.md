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
| `mach-o/dyld.h` | `_NSGetExecutablePath` | 返回 `$DM_ROOT/D2R`，让上游拼出的 `/dm/derived.tsv` 落到 `$DM_ROOT/dm/` | 换落点 |
| `dispatch/dispatch.h` | GCD —— **只做信号量**（`dispatch_semaphore_*` / `dispatch_time` / `dispatch_release`），pthread 实现 | 见第六节：block 那块**刻意不做** | 真实现（局部） |
| `shim_prefix.h` | 一批散用符号 | `strlcpy`/`strlcat`/`arc4random*`/`renamex_np`/`st_mtimespec`/`CLOCK_UPTIME_RAW`，以及**靠链接探针才发现的** `clock_gettime_nsec_np` / `pthread_get_stackaddr_np` / `pthread_get_stacksize_np` | 真实现 |

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
共 71 个 .c，通过 68   未通过 3
```

**未通过的 3 个是同一回事，而且不是移植问题**：

| 文件 | 卡在哪 |
|---|---|
| `diag.c` / `dm_bright.c` / `dm_vault_ui.c` | 缺 `dm_hud_exp.h` / `dm_bright_levels.inc` / `dm_vault_tab.h` —— 这三张表要用**你自己的游戏文件**生成（阶段 2） |

**也就是说：宿主侧已经全部编过，剩下 3 个卡在游戏文件上。**

> `hle_audio.c`（音频后端）已通过收口解决：苹果的 CoreAudio 部分被 `#if !defined(__OHOS__)` 关起来，
> 4 个调用点收口成 `DM_AO_*` 宏，鸿蒙侧由 `dm/ohos/audio_ohos.c` 承接
> （**目前是诚实的空实现** ⇒ 一律走静音接收端，节奏与真机一致、没有声音）。
> 真出声是增量工作，方案见 `dm/ohos/音频后端方案-从CoreAudio到OHAudio.md`。

### 再往下一层：链接（`tools/link_probe.sh`）

`-fsyntax-only` 只验"每个文件自己讲得通"，**验不了"甲文件喊的名字乙文件里到底有没有"**。
所以另做了一个链接探针：把 83 个目标文件真的链一次。

```
83 个目标文件 → 只剩 39 个未解析符号，且全部可解释
  · 5 个来自图形层（dm/gfx，尚未开工）
  · 34 个来自 diag.c / dm_vault_ui.c（要用你自己的游戏文件生成数据表）
```

**链接这一层抓出了 4 个 `-fsyntax-only` 永远发现不了的问题**（其中一个是真 bug）：

| 问题 | 为什么要紧 |
|---|---|
| `clock_gettime_nsec_np`（4 个文件在用） | 苹果独有；**不声明也能编过**，编译器默认它返回 `int` ⇒ **64 位纳秒被静默截断**。会算错时间且不报错 —— 最坏的一类 bug |
| `entry.S` 的符号带前导下划线（Mach-O 约定） | ELF 上没有下划线 ⇒ 7 个符号全对不上。已加按平台的 `CSYM()` 宏，正文汇编一字未改 |
| `pthread_get_stack*_np` 两边口径相反 | 苹果给**栈顶**、Linux 给**栈底**；反了会把栈上地址当成代码页 |
| `ld.lld` 默认只报 20 条错误 | 第一次跑出来"正好 20 个"，关掉上限后真实数字是 74 —— **工具给的数字不等于全部真相** |

详见 **`dm/ohos/链接现状与缺口.md`**。

### 另外两件在编译探针之外、但已经验证掉的事

1. **登记表段机制（链路级验证通过）**。`hle.h` 的 `HLE` / `HLE_FORCE` 宏原本写的是苹果段名
   `__DATA,swhle` / `__DATA,swforce`；而 ELF 上段名必须是**合法的 C 标识符**，链接器才会生成
   `__start_<名>` / `__stop_<名>` 符号。鸿蒙侧改成 `dmswhle` / `dmswforce` 后实测：

   ```
   ✅ 链接成功
   dmswhle    32 字节 = 2 条目 × 16 字节    __start_dmswhle   0x3cb8  __stop_dmswhle   0x3cd8
   dmswforce  24 字节 = 1 条目 × 24 字节    __start_dmswforce 0x3cd8  __stop_dmswforce 0x3cf0
   ```

   > **顺手修掉一个真 bug**：`dm/ohos/map_image_ohos.c` 里的 `dm_ohos_force_table()` 一直在找
   > `__start_dmswforce`，但宏写的却是 `__DATA,swforce`——**两边的名字从来没对上过**，
   > 强制接管表在鸿蒙侧永远是空的。空表的后果不是"少接管几个函数"，而是
   > `nn::fs::Commit` 这类服务会解析到 SDK 自己的实现、去找鸿蒙上根本不存在的 fsp-srv 服务。
   > 现在对上了。


---

## 五、还没做、也不打算在本层做的

| 缺的东西 | 为什么不做在这儿 |
|---|---|
| **GCD 里收 block 的那几个**（`dispatch_after` / `dispatch_source_set_event_handler` / `dispatch_async`…） | 收的是 block 字面量。block 要"存下来稍后再调"就必须复制到堆上，也就是需要一套 **block 运行时**（`_Block_copy` / `_Block_object_assign` / `_NSConcreteStackBlock`）。鸿蒙 sysroot 里既没有 `Block.h` 也没有这个运行时库。自己手搓，是**在没有真机验证的情况下赌**——赌错的后果是"回调跳进一块已经失效的栈内存"，属于最难查的那类崩溃。所以不赌：宿主里唯一用到它的 `hle_lifecycle.c` 在鸿蒙侧改用 pthread 直写那两处（一个延时退出线程 + 一个 `sigwait` 线程），语义反而更直白 |
| `AudioToolbox` | 不是"垫"的事，是**换一个音频后端**（CoreAudio AudioUnit → 鸿蒙音频服务） |
| `CoreFoundation` | `dm_i18n.c` 里已被 `#if TARGET_OS_IPHONE` 守卫排除（垫片的 `TargetConditionals.h` 让它自动落到鸿蒙侧），无需处理 |
| 上游直接写的 `fprintf(stderr, ...)` | `dm.h` 的 `LOG()` 已接 hilog，但少数地方直接写 stderr；真机上 stderr 不一定可见，另案处理 |

> GCD 这块可以再说清一点：**信号量部分是"真实现"，block 部分不是"没做"而是"刻意不做"**。
> 这两者的区别很重要——前者能验证（`dispatch_semaphore_wait` 成功返回 0、超时返回非 0，
> 与 GCD 一致）；后者在鸿蒙上无法验证，而无法验证的东西宁可换成能看懂的写法。

> 关于 `is_write`（`crash.c`）的一个说明，值得单独记一笔：
> 苹果能从不变量寄存器 ESR 的 WnR 位读出"这次缺页是读还是写"，Linux/aarch64 的 `sigcontext` 里没有 ESR。
> 但这里真正要的只有一件事——**这页是不是被我们改成只读了**（图形层 `nvn_dirty` 的缺页钩子靠它判断）。
> 只读页上的"读"根本不会缺页，所以凡是在这种页上缺页，**一定是写**：这是按语义推出来的，不是猜的。
> 而且偏"写"正好是安全侧——偏了只是白解一次页保护；偏成"读"会让钩子返回 0，游戏在写只读页时直接崩。

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
7. **`crash.c` 里那个 `is_write` 的取值**：鸿蒙侧我固定取 1（理由见下一节），
   要在真机上人为造一次"写只读页"的缺页，确认图形层的缺页钩子能正常解开保护、而不是崩掉。
8. **`hle_fdstat.c` 的 `/proc/self/fd/<n>` 读取**：被 seccomp 拦的话，DM_FD_LOG 的诊断行里
   路径会全空（归类成"其它"），不影响游戏，但会失去这条诊断的价值。

---

## 七、这次改到的上游文件（就这几个，逐条列明）

垫片层本身是**零改动**的。但下面几处靠垫片解决不了（要么是段名这种编译期属性，
要么是两边结构不同必须按字段改），所以动了上游文件——**每一处都是用
`#if defined(__OHOS__)` 分叉，苹果那边的展开结果与改动前逐字符相同**：

| 上游文件 | 改了什么 | 为什么垫片解决不了 |
|---|---|---|
| `dm/host/hle.h` | 段名改成由宏 `DM_HLE_SECTION` / `DM_FORCE_SECTION` 提供，鸿蒙侧为 `dmswhle` / `dmswforce` | 段名是编译期属性，无法在运行时替换；且 ELF 要求段名是合法 C 标识符 |
| `dm/host/hle.c` | `mach-o/*` 的两个 include 加守卫；登记表改为经 `__start_dmswhle`/`__stop_dmswhle` 取 | 同 loader.c 已有的写法 |
| `dm/host/crash.c` | 新增 `DM_UC_PC/LR/FP/SP/X` 五个取寄存器宏；缺页的读/写判断加鸿蒙分支 | 两边 `ucontext_t` 一个是**指针**、一个是**值**，字段名全不同 |
| `dm/host/hle_fdstat.c` | 新增 `DM_FD_PATH` 宏（鸿蒙走 `/proc/self/fd/<n>`） | 苹果的 `F_GETPATH` 是 fcntl 扩展，Linux 没有；`fcntl` 是可变参数，垫不了 |
| `dm/host/hle_lifecycle.c` | 两处 dispatch（`dispatch_after` + `dispatch_source_*`）在鸿蒙侧改用 pthread 直写 | 它们收的是 block，见第五节——块运行时无法验证，所以换成能看懂的写法 |
| `dm/host/entry.S` | 加了 `CSYM()` 符号名宏（鸿蒙 `x` / 苹果 `_x`） | ELF 与 Mach-O 的 C 符号命名约定不同；汇编无法"垫"，只能按平台绑名。**正文指令一字未改** |
| `dm/host/dm.h` | 日志宏加 `DM_LOG_TO_HILOG` 守卫 | 宏定义只能在一处收口 |

另外一处**没动源码**：`dm_textbox_core.c` 用了 `memmem`，在苹果上由 `<string.h>` 顺带声明，
musl 要 `_GNU_SOURCE` 才声明——所以只是在构建里加了这个宏定义。

`dm/host/dm_derived_rt.h` **一个字没改**，靠 `compat/mach-o/dyld.h` 垫片解决。
