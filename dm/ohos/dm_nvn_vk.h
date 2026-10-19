// 通用 NVN 内存池/缓冲胶水层（L1，与游戏无关）。
// General NVN memory-pool / buffer glue layer: platform-neutral, per the pivot the
// NVN implementations must live in a framework module, not inside the probe.
//
// 从探针 dm_probe.c 整体搬来（2026-10-19，架构转向后的 G2 收尾）——原为 Task #2 的实现。
// 鸿蒙是 UMA（CPU/GPU 同一块内存）⇒ 池/缓冲都直接 malloc 备内存，GetAddress 返回真地址。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void dm_nvn_vk_set_log(void (*fn)(const char *, ...));

// 按 NVN 函数名取内存池/缓冲族的真实现；不是本模块管的返回 NULL。
void *dm_nvn_vk_real(const char *name);

#ifdef __cplusplus
}
#endif
