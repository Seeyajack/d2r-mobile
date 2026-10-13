// 垫片：苹果 <mach/mach_time.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <mach/mach_time.h>.
//
// 语义对齐：苹果在 arm64 上 mach_timebase_info 恒为 1/1，也就是说 mach_absolute_time()
// 直接以纳秒为单位。所以我们用 CLOCK_MONOTONIC 的纳秒直接返回，**数值语义与苹果一致**，
// 上游 `mach_absolute_time() * tb.numer / tb.denom` 的算法不用改、结果也不变。
// Semantics: on arm64 Apple's mach_timebase_info is always 1/1, i.e. mach_absolute_time() is already in
// nanoseconds. So we return CLOCK_MONOTONIC nanoseconds directly -- numerically identical to Apple --
// and upstream's `mach_absolute_time() * tb.numer / tb.denom` needs no change and yields the same result.
//
// 实现见 src/dm_compat_mach.c。
#pragma once

#include "dm_compat_types.h"

typedef struct mach_timebase_info_data_s {
    uint32_t numer;
    uint32_t denom;
} mach_timebase_info_data_t;

uint64_t mach_absolute_time(void);
kern_return_t mach_timebase_info(mach_timebase_info_data_t *tb);
