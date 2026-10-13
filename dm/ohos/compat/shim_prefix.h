// 垫片前缀头：靠 -include 强制包含进每一个编译单元。
// Shim prefix header: force-included into every translation unit via -include.
//
// 为什么需要它：有些苹果独有的 libc 函数，上游源码是"直接用、不额外包含头文件"的
// （在苹果上它们由 <string.h> / <stdlib.h> 顺带声明）。鸿蒙用的 musl libc 没有这些符号，
// 于是会产生"隐式声明"警告甚至错误。用一个强制包含的头统一声明，比改 22 个文件的 include 干净。
// Why: some Apple-only libc functions are used by upstream without any extra include (on Apple they are
// declared through <string.h> / <stdlib.h>). musl libc on HarmonyOS has none of them, giving implicit
// declaration warnings or errors. One force-included header is cleaner than editing 22 files' includes.
//
// 涉及的符号（都做过全宿主扫描，见《适配清单-宿主层.md》的普查表）：
//   strlcpy            22 个文件
//   strlcat             1 个文件
//   arc4random 系列     3 个文件（dm_namegen.c / dm_rift.c / hle_os.c）
//   renamex_np          1 个文件（hle_fs.c，存档提交时的原子互换）
// 实现见 src/dm_compat_str.c，全部用 __attribute__((weak)) 定义：
// 万一鸿蒙 libc 将来自带同名符号，链接时以它的为准，不会打架。
// Implementations are in src/dm_compat_str.c, all defined __attribute__((weak)) so that if the HarmonyOS
// libc ever ships the same symbol, its version wins at link time without a clash.
#pragma once

// 本文件全部内容对汇编文件无意义，所以先挡一层：
// 构建里对**所有源文件**统一 `-include shim_prefix.h`（.S 也在内），不必为汇编单独开一套选项。
// 汇编器不认 <time.h> / <pthread.h> 这些 C 头——一开始 `entry.S` 编不过就是这个原因。
// Everything below is meaningless to an assembler, so it is fenced off here: the build force-includes
// shim_prefix.h into every translation unit including .S files, so no separate option set is needed for
// assembly. The assembler rejects C headers like <time.h> / <pthread.h> -- that is exactly why entry.S
// failed to compile at first.
#if defined(__ASSEMBLER__) || defined(__ASSEMBLY__)
// 汇编文件：什么都不做 | Assembly: do nothing.
#else

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

// 苹果独有的三个函数，靠 <time.h> / <pthread.h> "顺带声明"，所以按头文件普查是数不到的。
// 它们由**链接探针**抓出来（`tools/link_probe.sh`）：只做 `-fsyntax-only` 永远发现不了。
// Three Apple-only functions that arrived through <time.h> / <pthread.h>, invisible to a header census.
// They were caught by the link probe (tools/link_probe.sh); -fsyntax-only can never find them.
//
// ⚠️ 有一条特别要命：`clock_gettime_nsec_np` 不声明也能编过（C 只给警告），
// 而**编译器会默认它返回 int**——64 位纳秒值当场被截断到 32 位，而且是静默的。
// 宿主有 4 个文件在用它（dm_bossrush.c / dm_textbox.c / hle_fs.c / hle_time.c）。
// ⚠️ One is especially nasty: clock_gettime_nsec_np compiles without a declaration (C only warns), and
// the compiler assumes it returns int -- silently truncating 64-bit nanoseconds to 32 bits. Four host
// files call it (dm_bossrush.c / dm_textbox.c / hle_fs.c / hle_time.c).
uint64_t clock_gettime_nsec_np(clockid_t clock_id);

// 苹果的栈信息接口：返回调用线程的**栈顶**（高地址）与栈大小。
// Linux 的等价物是 pthread_getattr_np + pthread_attr_getstack（给的是**低**地址），
// 所以实现里要换算一下，见 src/dm_compat_mach.c。hle_svc.c 拿它来回答"这个地址是不是在主线程栈上"。
// Apple's stack introspection: the stack base (high address) and size of a thread. The Linux equivalent
// is pthread_getattr_np + pthread_attr_getstack, which yields the LOW address, so the implementation
// converts; see src/dm_compat_mach.c. hle_svc.c uses it to answer "is this address on the main stack?".
// 返回 NULL / 0 = 取不到（调用方本来就写了"取不到就按别的方式答"的分支）。
// NULL / 0 means unavailable (the caller already handles that).
void  *pthread_get_stackaddr_np(pthread_t thread);
size_t pthread_get_stacksize_np(pthread_t thread);

// 有些上游文件用了 TARGET_OS_IPHONE / TARGET_OS_MAC，却没有自己包含 <TargetConditionals.h>
// （在苹果上靠别的头间接带进来）。这里统一带上，顺手把这个坑填平——hle_fs.c:311 就是这种情况。
// Some upstream files use TARGET_OS_IPHONE / TARGET_OS_MAC without including <TargetConditionals.h>
// themselves (on Apple it arrives indirectly). Bringing it in here fills that hole -- hle_fs.c:311 is
// exactly this case.
#include <TargetConditionals.h>

// 下面两条都是"同物异名"，不是功能差异：
// Both of these are the same thing under a different name, not a functional difference:
//
//   苹果 st_mtimespec  ←→  Linux st_mtim        （修改时间字段名；只有名字不同）
//   苹果 CLOCK_UPTIME_RAW ←→ Linux CLOCK_MONOTONIC（都不含休眠时间，语义对齐）
//     注意别错配成 CLOCK_BOOTTIME——那个把休眠算进去，会让"经过了多少时间"变长。
//     Do not mismatch it with CLOCK_BOOTTIME, which counts suspend time and would make elapsed time longer.
#ifndef st_mtimespec
#define st_mtimespec st_mtim
#endif
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif

// 苹果在 <stdio.h> 里声明的 renamex_np 及其标志位（Darwin 头文件 <sys/stdio.h>）。
// Apple declares renamex_np and its flags in <stdio.h> (Darwin <sys/stdio.h>).
#define RENAME_SWAP 0x00000002
#define RENAME_EXCL 0x00000004

#ifdef __cplusplus
extern "C" {
#endif

size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);

uint32_t arc4random(void);
uint32_t arc4random_uniform(uint32_t upper_bound);
void     arc4random_buf(void *buf, size_t n);

int renamex_np(const char *from, const char *to, unsigned int flags);

#ifdef __cplusplus
}
#endif

#endif   // !__ASSEMBLER__
