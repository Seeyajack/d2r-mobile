// 时钟：苹果那套 mach 时钟 + clock_gettime_nsec_np。
// Clocks: Apple's mach clocks plus clock_gettime_nsec_np.
//
// 这三样都是"靠 <time.h> / <mach/*> 顺带声明"的，所以按头文件普查数不到——
// 垫片层是靠**链接探针**才发现漏了的（见 dm/ohos/compat/README.md 第二节问题 1）。
// All three arrive indirectly through <time.h> / <mach/*>, invisible to a header census; the shim layer
// only learned it was missing one of them from the link probe.
#include "demoapp.h"

#include <mach/mach_time.h>
#include <mach/mach.h>
#include <time.h>

uint64_t demo_mono_ns(void) {
    // 苹果写法：mach_absolute_time × 时基；arm64 上时基恒为 1/1
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

uint64_t demo_uptime_ns(void) {
    // 苹果独有函数 + 苹果独有的时钟 id（不含休眠的单调时钟）
    return (uint64_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

uint64_t demo_cpu_ns(void) {
    return (uint64_t)clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
}
