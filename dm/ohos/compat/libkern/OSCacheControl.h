// 垫片：苹果 <libkern/OSCacheControl.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <libkern/OSCacheControl.h>.
//
// 宿主用它做一件事：**写完代码之后刷指令缓存**（sys_icache_invalidate）。
// 移植后改写代码、把数据页变成可执行页之后必须刷，否则 CPU 可能执行到旧指令。
// The host uses it for one thing: invalidating the instruction cache after writing code.
//
// 鸿蒙上不需要手写汇编：编译器内建 __builtin___clear_cache 会按目标平台生成正确的序列
// （aarch64 上是 dc cvau / ic ivau / dsb / isb）。这是官方推荐的可移植写法。
// No hand-written assembly is needed: the compiler builtin __builtin___clear_cache emits the correct
// sequence for the target (dc cvau / ic ivau / dsb / isb on aarch64). This is the portable way to do it.
#pragma once

#include <stddef.h>

static inline void sys_icache_invalidate(void *start, size_t len) {
    __builtin___clear_cache((char *)start, (char *)start + len);
}

static inline void sys_dcache_flush(void *start, size_t len) {
    __builtin___clear_cache((char *)start, (char *)start + len);
}

static inline void OSMemoryBarrier(void) {
    __sync_synchronize();
}
