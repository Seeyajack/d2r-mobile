// 通用 NVN 胶水层（L1）：设备族等"平台中立"的 NVN 语义。
// General NVN glue layer (L1): platform-neutral NVN semantics such as the device family.
//
// 定位与 dm_sdk_ohos.c 相同：这些实现**与游戏无关**（NVN 就是 Switch 的图形 API），换游戏一行不改；
// 探针只做分派。上游 dm/gfx/nvn/hle_nvn.c 有完整 203 个 API 的语义，但它绑死 Metal + mach
// （macOS）——所以这里按"能搬就搬"的原则，先把**纯数据/纯状态**的部分搬过来（设备属性表、
// 原点/深度模式记录、句柄换算），后续 Vulkan 后端（池/缓冲/贴图）在 dm_nvn_vk 一侧续建。
// Same positioning as dm_sdk_ohos.c: nothing here depends on any particular game, so switching
// games needs no change. Upstream hle_nvn.c has all 203 APIs but is welded to Metal + mach;
// this file migrates the pure-data / pure-state parts first (device info table, origin/depth
// mode, handle encoding), with the Vulkan-backed objects following on the dm_nvn_vk side.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dm_nvn_set_log(void (*fn)(const char *, ...));

// 按 NVN 函数名取真实现；不是本模块管的返回 NULL（调用方回落记账桩）。
void *dm_nvn_real(const char *name);

#ifdef __cplusplus
}
#endif
