// 鸿蒙侧**共享 Vulkan 上下文** + 客体堆预留 —— 阶段 3（图形层回放）的地基。
// The shared Vulkan context and the guest-heap reservation on HarmonyOS -- the foundation of phase 3.
//
// 为什么要有「共享上下文」：VkDeviceMemory 是**绑定到某一个 VkDevice** 的 —— 设备 A 分配的内存，
// 设备 B 的缓冲绑不上。而客体堆必须和将来的图形层（`nvn_vk.c`）用**同一个设备**；更要紧的是，
// 它必须在「游戏要堆」之前就存在：游戏启动时 `SetHeapSize` 就要一整块约 3.7 GB，那时图形层还没初始化。
// 所以这里做一个**全进程唯一、按需创建**的上下文；首个使用者很可能就是客体堆预留。
// Why a shared context: VkDeviceMemory is bound to a single VkDevice, and the guest heap must live on
// the same device as the graphics layer -- moreover it must exist *before* the game asks for its heap
// (SetHeapSize happens at startup, earlier than any graphics initialisation). Hence one process-wide,
// created-on-demand context whose first user is likely the heap reservation.
//
// 它与 `dm/ohos/vk_backend.c` 的关系：那个是**阶段 1/2 的离屏测试后端**（自带上下文，将来由
// `nvn_vk.c` 取代）；这里是**生产路径**要用的那一个。两者目前只在探针 App 里并存。
// Relation to vk_backend.c: that one is the phase 1/2 offscreen test backend (its own context, to be
// replaced by nvn_vk.c); this one is what the production path uses. They coexist only inside the probe app.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// 按需创建。任一失败都返回 VK_NULL_HANDLE / 0，原因见 dm_ohos_vk_err()。
VkInstance       dm_ohos_vk_instance(void);
VkPhysicalDevice dm_ohos_vk_phys(void);
VkDevice         dm_ohos_vk_device(void);
uint32_t         dm_ohos_vk_qfam(void);
VkQueue          dm_ohos_vk_queue(void);
VkCommandPool    dm_ohos_vk_pool(void);       // 共享命令池（回放层要自己录一段多命令的缓冲）

// 有队列的族（真机实测：**图形族不支持 present**，得用另一个族来提交出图）
uint32_t         dm_ohos_vk_qfam_count(void);
uint32_t         dm_ohos_vk_qfam_at(uint32_t i);      // 第 i 个族的族号
VkQueue          dm_ohos_vk_queue_at(uint32_t i);     // 第 i 个族的队列
const char      *dm_ohos_vk_err(void);        // 创建失败的原因（没失败就是空串）
const char      *dm_ohos_vk_devname(void);

// 一次性命令缓冲：record 里录命令，提交并等完成。0 = 成功。
int dm_ohos_vk_oneshot(void (*record)(VkCommandBuffer, void *), void *ud);

// 客体堆预留：向共享上下文要一块 size 字节、**CPU 可写 + 显存本地 + 一致**的内存并映射。
// 成功返回 CPU 映射地址（今后就是客体堆基址），失败返回 NULL（原因见 dm_ohos_vk_err()）。
// 重复调用返回同一块（幂等）。
// Guest-heap reservation: allocate size bytes of HOST_VISIBLE|DEVICE_LOCAL|HOST_COHERENT memory from the
// shared context and map it. Returns the CPU address (the future heap base), or NULL on failure.
void *dm_ohos_heap_reserve(uint64_t size);
void  dm_ohos_heap_release(void);

// 预留出来的那块 VkDeviceMemory（池切片要 `vkBindBufferMemory` 绑到它上面）与它的地址/大小。
// The VkDeviceMemory behind the reservation (pools bind onto it) plus its address and size.
VkDeviceMemory dm_ohos_heap_memory(void);
void          *dm_ohos_heap_base(void);
uint64_t       dm_ohos_heap_size(void);
uint32_t       dm_ohos_heap_mem_type(void);   // 客体堆用的内存类型序号（池缓冲要校验它）

#ifdef __cplusplus
}
#endif
