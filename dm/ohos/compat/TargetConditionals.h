// 垫片：苹果 <TargetConditionals.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <TargetConditionals.h>.
//
// 用法：把本目录放在 include 路径最前面（-I .../dm/ohos/compat），宿主源码里的
// #include <TargetConditionals.h> 就会命中这里——上游文件一个字都不用改。
// Usage: put this directory first on the include path (-I .../dm/ohos/compat); `#include <TargetConditionals.h>`
// in the host sources then lands here, and not a single character of the upstream files has to change.
//
// 判据：鸿蒙不是苹果，所以 TARGET_OS_MAC / TARGET_OS_IPHONE 一律为 0。源码里
// `#if TARGET_OS_IPHONE ... #elif defined(__OHOS__) ...` 这种写法会自然落到鸿蒙那一侧
// （loader.c 的 map_image 就是靠这个分叉的）。
// Rationale: HarmonyOS is not Apple, so TARGET_OS_MAC / TARGET_OS_IPHONE are all 0. Code such as
// `#if TARGET_OS_IPHONE ... #elif defined(__OHOS__) ...` then falls to the HarmonyOS side by itself
// (this is exactly how map_image in loader.c branches).
#pragma once

#define TARGET_OS_MAC            0
#define TARGET_OS_OSX            0
#define TARGET_OS_IPHONE         0
#define TARGET_OS_IOS            0
#define TARGET_OS_TV             0
#define TARGET_OS_WATCH          0
#define TARGET_OS_VISION         0
#define TARGET_OS_BRIDGE         0
#define TARGET_OS_DRIVERKIT      0
#define TARGET_OS_SIMULATOR      0
#define TARGET_OS_EMBEDDED       0
#define TARGET_OS_MACCATALYST    0
#define TARGET_OS_WIN32          0
#define TARGET_OS_LINUX          1
#define TARGET_OS_UNIX           1

// 我们自己认得的那一根。__OHOS__ 由编译器的目标三元组自动定义（aarch64-unknown-linux-ohos），
// 这里只兜底，避免极端情况下漏定义导致 loader.c 的分叉跑错边。
// Our own switch. __OHOS__ is defined automatically by the compiler target triple
// (aarch64-unknown-linux-ohos); this is only a fallback.
#ifndef __OHOS__
#define __OHOS__ 1
#endif

#define TARGET_OS_OHOS           1
#define TARGET_RT_64_BIT         1
#define TARGET_CPU_ARM64         1
#define TARGET_CPU_ARM64E        0
#define TARGET_CPU_ARM           0
#define TARGET_CPU_X86_64        0
