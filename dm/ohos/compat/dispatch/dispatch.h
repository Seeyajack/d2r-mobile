// 垫片：苹果 <dispatch/dispatch.h>（GCD）的鸿蒙替身——**只覆盖宿主真正用到的那部分**。
// Shim: the HarmonyOS stand-in for Apple's <dispatch/dispatch.h> (GCD), covering only what the host uses.
//
// 宿主里只有 2 个文件用它，而且用途完全不同，所以做法也分两种：
// Only two host files use it, for entirely different purposes, so they are handled differently:
//
//   hle_audio.c —— 只用信号量与 dispatch_release，**完全不涉及 block**。
//                  → 这里给一份基于 pthread 的真实现（互斥量 + 条件变量），语义与 GCD 一致。
//      Uses only semaphores and dispatch_release, with no blocks at all -> gets a real pthread-backed
//      implementation (mutex + condition variable) with the same semantics as GCD.
//
//   hle_lifecycle.c —— 用 dispatch_after 与 dispatch_source_* + **block 字面量（^{...}）**。
//                      → 本层**不做**，理由见下；该文件在鸿蒙侧改用 pthread 直写那两处。
//      Uses dispatch_after and dispatch_source_* with block literals -> deliberately NOT provided here;
//      that file writes those two spots with pthread directly on HarmonyOS.
//
// 为什么不做 block 那块（这是本次唯一一处"刻意不做"的决定）：
//   dispatch_source_set_event_handler / dispatch_after 收的是 block。block 一旦要"存下来稍后再调"
//   （信号源正是如此），就必须真的复制到堆上，也就是需要一套 **block 运行时**
//   （_Block_copy / _Block_object_assign / _NSConcreteStackBlock…）。
//   鸿蒙 sysroot 里既没有 Block.h 也没有这个运行时库。自己手搓一套，是**在没有真机验证的情况下赌**——
//   赌错的后果是"回调跳到一块已经失效的栈内存上"，属于最难查的那类崩溃。
//   所以这里不赌：让 hle_lifecycle.c 在鸿蒙侧直接用 pthread 写那两处。语义反而更直白
//   （一个延时退出的线程 + 一个 sigwait 线程），而且不引入任何不确定的东西。
// Why the block part is missing: dispatch_source_set_event_handler / dispatch_after take blocks, and a
// block that must survive the call (exactly what a signal source needs) has to be copied to the heap,
// requiring a block runtime (_Block_copy, _Block_object_assign, _NSConcreteStackBlock...). The HarmonyOS
// sysroot ships neither Block.h nor that runtime. Hand-rolling one would be a gamble with no way to
// verify it on device, and losing that gamble means "a callback jumping into stack memory that is already
// dead" -- among the hardest crashes to find. So this does not gamble: hle_lifecycle.c writes those two
// spots with pthread on HarmonyOS, which is clearer anyway and introduces nothing uncertain.
#pragma once

#include <pthread/qos.h>   // QOS_CLASS_*：与苹果一样，这些常量来自 pthread/qos.h | As on Apple, these come from pthread/qos.h
#include <stdint.h>
#include <stddef.h>

// 不透明类型：与 GCD 一样，调用方只持有指针
// Opaque types: as in GCD, callers only ever hold pointers
typedef struct dm_dispatch_semaphore_s *dispatch_semaphore_t;
typedef struct dm_dispatch_queue_s     *dispatch_queue_t;

// 时间基：苹果用"宿主时钟的绝对纳秒"（不随系统时间跳变），这里用 CLOCK_MONOTONIC 的纳秒，同性质。
// Time base: Apple uses absolute host-clock nanoseconds (unaffected by wall-clock changes); we use
// CLOCK_MONOTONIC nanoseconds, which has the same property.
typedef uint64_t dispatch_time_t;

#define DISPATCH_TIME_NOW     ((dispatch_time_t)0)
#define DISPATCH_TIME_FOREVER (~(dispatch_time_t)0)

#define NSEC_PER_SEC  1000000000ull
#define NSEC_PER_MSEC 1000000ull
#define NSEC_PER_USEC 1000ull

dispatch_time_t  dispatch_time(dispatch_time_t when, int64_t delta);
dispatch_queue_t dispatch_get_global_queue(long identifier, unsigned long flags);

dispatch_semaphore_t dispatch_semaphore_create(long value);
long dispatch_semaphore_signal(dispatch_semaphore_t dsema);
long dispatch_semaphore_wait(dispatch_semaphore_t dsema, dispatch_time_t timeout);

void dispatch_retain(void *object);
void dispatch_release(void *object);
