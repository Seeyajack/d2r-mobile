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

#include <stddef.h>
#include <stdint.h>
#include <time.h>

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
