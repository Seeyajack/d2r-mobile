// 鸿蒙侧回放层（`nvn_vk.c`）—— 对标上游 `nvn_mtl.m`（Metal 回放层）。**第一步只有内存池这一块。**
// The HarmonyOS replay layer, the counterpart of upstream's nvn_mtl.m. Step 1 covers memory pools only.
//
// 为什么池排第一：游戏的顶点、贴图、渲染目标、命令内存**全都住在内存池里**。回放层要做的第一件事，
// 就是把"游戏给的一个地址"翻译成"GPU 能用的缓冲 + 偏移"——上游 `mtl_addr` / `mtl_gpu_addr` 干的就是这个。
// Why pools first: everything the game feeds the GPU lives in memory pools. The replay layer's first job
// is translating a guest address into a GPU buffer + offset -- exactly what mtl_addr / mtl_gpu_addr do.
//
// 与 Metal 的**刻意不同**：
//   ① **没有"驻留集 / 按需驻留"那一套**。上游那套是为 iOS 的 `MTLResidencySet` 而生（把整池读进内存、
//      钉成 wired、不计入 footprint，512 MB 渲染目标池一次没用过也占 530 MB）。Vulkan 没有这个语义，
//      内存一分配就在。所以这里整块省掉——**不是漏了，是不需要**。
//   ② **零拷贝不靠"套壳"**，靠的是**客体堆本身就是 Vulkan 内存**（`dm/ohos/vk_ctx_ohos.c`）。
//      所以池登记 = 在这块内存上建一个 `VkBuffer`、绑到池的偏移上；全程没有任何拷贝。
// Two deliberate differences from Metal: no residency set / on-demand residency (an iOS concept), and
// zero-copy comes from the guest heap itself being Vulkan memory rather than from wrapping host memory.
//
// **尚未做（后续小步）**：贴图要 `VkImage` 而不是 `VkBuffer`，且 Vulkan 不允许缓冲与图像**同时**
// 占用同一段内存（有 `bufferImageGranularity` 约束）——所以池里同时住着缓冲和贴图这件事，要在贴图那一步
// 单独设计（分槽 / 分区登记）。本步只覆盖"缓冲"。
// Not yet done (later steps): textures need VkImage, and Vulkan forbids a buffer and an image overlapping
// the same memory range, so pools holding both need a dedicated design in the texture step.
#pragma once

#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// 池登记 / 注销 —— 对标 `mtl_pool_add` / `mtl_pool_del`。
// 返回 0 = 成功；非 0 = 失败（池不在客体堆里、建缓冲失败……），原因见 nvn_vk_last_error()。
int  nvn_vk_pool_add(const void *mem, uint64_t size);
void nvn_vk_pool_del(const void *mem);

// 地址反查 —— 对标 `mtl_addr` / `mtl_gpu_addr`。
// `addr..addr+len` 整个落在某个池里时：`*buf` = 该池的 VkBuffer，`*off` = `addr − 池基址`
// （就是绑定点/动态偏移要用的那个数），`*gpu` = 该地址的设备地址。
// 返回 1 = 命中，0 = 没有池覆盖这个区间（调用方据此报错）。
int  nvn_vk_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off);
int  nvn_vk_gpu_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *gpu);

// 诊断
int          nvn_vk_pool_count(void);
const char  *nvn_vk_last_error(void);

#ifdef __cplusplus
}
#endif
