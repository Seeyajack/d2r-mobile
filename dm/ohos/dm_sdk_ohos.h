// 通用 SDK 胶水层（L1）：把 Switch SDK 的导入语义按**模块**组织成"符号名 → 实现"表。
// General SDK glue layer (L1): organises Switch SDK import semantics per module into a
// "symbol name -> implementation" table.
//
// 为什么单独成文件：这些语义**与游戏无关**（vi 就是显示器、oe 就是运行环境），换游戏一行不用改；
// 而探针（dm_probe.c）是一次性验证工具。两者混在一起，"通用框架"就退化成"某款游戏的补丁集"。
// Why a separate file: these semantics have nothing to do with any particular game, so switching
// games requires no changes here; the probe is a one-off harness. Mixing them turns the "general
// framework" into a patch set for one game.
//
// 与 dm_fs_ohos.c 的分工：fs 需要文件表/归档后端，单独成模块；本文件收 vi/oe 这类
// "轻量语义"模块，后续 nn::os 事件、nn::pctl 等也往这里加（按模块分节，保持可读）。
// Division of labour with dm_fs_ohos.c: fs needs a file table / archive backend, so it lives
// on its own; this file takes lightweight semantic modules (vi/oe), and later nn::os events,
// nn::pctl etc. join here, one section per module.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 日志出口（探针把 klog 接进来；不接则不打印）。
void dm_sdk_set_log(void (*fn)(const char *, ...));

// 按导入符号名分派。返回 1 = 已处理（*ret 为返回值），0 = 不归本层管（调用方回落记录型存根）。
// Dispatch by imported symbol name. Returns 1 when handled (*ret holds the return value),
// 0 when the symbol is not ours (the caller falls back to the recording stub).
// np96：第 4 个参数（x3）也转发——VSNPrintf 的 va_list 指针在 x3，丢掉它 = 格式化必崩。
int dm_sdk_call(const char *sym, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                uint64_t *ret);

// C++ 静态局部对象初始化守卫（__cxa_guard_*）的统计摘要。
// 探针在**游戏自己的帧线程**上调用（仪器铁律：观测绝不挂宿主线程）。
void dm_sdk_guard_report(void);

// 注入主映像的 R+X 封区（写 guest 内存前的可写性判据之一，见实现注释）。
// base = 映像运行期基址；ranges[i] = { 映像内偏移 lo, hi }，左闭右开。
// 不注入也能工作（退化为只认 /proc/self/maps，查不到则放行），但注入后判断更准。
void dm_sdk_set_sealed_ranges(uint64_t base, const uint64_t (*ranges)[2], int n);

#ifdef __cplusplus
}
#endif
