// 垫片：苹果 <malloc/malloc.h> 的鸿蒙替身（宿主只用到 malloc_zone_statistics）。
// Shim: the HarmonyOS stand-in for Apple's <malloc/malloc.h>. The host uses only malloc_zone_statistics.
//
// 用途（diag.c，2 处）：长时间跑机时的内存观测表，把"malloc 已用字节"按 MiB 打进日志/TSV。
// 这是**观测**，不参与任何游戏逻辑。
// Purpose (two places in diag.c): the long-run soak memory table, printing allocated bytes in MiB.
// This is observation only and feeds no game logic.
//
// 鸿蒙侧的现实：musl 没有苹果的 malloc zone 概念，也没有稳定的等价接口
// （mallinfo 在 musl 上不作为标准接口存在，读了可能拿到假数据）。所以这里**如实降级**：
// 填 0，并打一条一次性说明。宁可日志里写 0 并说明原因，也不填一个看着像真的假数字——
// 那种数字会让人在排查内存问题时被误导。
// Reality on HarmonyOS: musl has no malloc-zone concept and no stable equivalent. So we degrade
// honestly: fill zeros and log a one-off explanation. Better a zero with a reason than a plausible
// fake number that would mislead someone chasing a memory problem.
#pragma once

#include <stddef.h>

typedef struct malloc_statistics_s {
    unsigned blocks_in_use;
    size_t   size_in_use;
    size_t   max_size_in_use;
    size_t   size_allocated;
} malloc_statistics_t;

// 不完整类型：宿主只会传 NULL（"全部 zone"）。
// Incomplete type: the host only ever passes NULL ("all zones").
typedef struct malloc_zone_t malloc_zone_t;

void malloc_zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats);
