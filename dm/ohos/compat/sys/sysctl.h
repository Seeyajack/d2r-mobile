// 垫片：苹果 <sys/sysctl.h> 的鸿蒙替身（宿主只用到一件：KERN_PROC_PID 取进程启动时刻）。
// Shim: the HarmonyOS stand-in for Apple's <sys/sysctl.h>. The host uses exactly one thing:
// KERN_PROC_PID to get the process start time.
//
// 语义：main.c 的 proc_start_mono_ns() 拿 p_starttime（墙钟）减去当前墙钟，算出"距进程启动多久"，
// 再把 now_ns() 的单调时钟零点对齐到进程创建那一刻。鸿蒙上没有任何东西能直接给这个值，
// 但 /proc/self/stat 的第 22 字段（系统启动后经过的时钟滴答数）+ /proc/uptime 可以算出来，
// 见 src/dm_compat_fs.c。**算不出来就返回 -1**——调用方本来就写了回退（返回 mono），不会崩。
// Semantics: proc_start_mono_ns() derives the age of the process from p_starttime, aligning the monotonic
// zero point to process creation. On HarmonyOS this is computed from field 22 of /proc/self/stat plus
// /proc/uptime (see src/dm_compat_fs.c). If it cannot be computed we return -1, and the caller already
// has a fallback (returns mono), so nothing crashes.
#pragma once

#include <stddef.h>
#include <sys/time.h>

#define CTL_KERN        1
#define CTL_MAXNAME     12
#define KERN_PROC       14
#define KERN_PROC_PID   1

// 只放宿主真正读到的字段：kp_proc.p_starttime。
// Only the field the host actually reads: kp_proc.p_starttime.
struct kinfo_proc {
    struct {
        struct timeval p_starttime;
    } kp_proc;
};

int sysctl(const int *name, unsigned int namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen);
