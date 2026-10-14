// 共享 Vulkan 上下文 + 客体堆预留的实现。接口与理由见 vk_ctx_ohos.h。
// Implementation of the shared Vulkan context and the guest-heap reservation; see vk_ctx_ohos.h.
#include "vk_ctx_ohos.h"

#include "dm_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 设备能力探测的结论：这台设备是 Vulkan 1.3（Maleoon 920）。按 1.3 建 instance/device。
#define DM_VK_API VK_API_VERSION_1_3

typedef struct {
    VkInstance       inst;
    VkPhysicalDevice phys;
    VkDevice         dev;
    uint32_t         qfam;
    VkQueue          queue;
    VkCommandPool    pool;
    char             err[256];
    char             devname[256];
    int              tried;          // 已经尝试过创建（失败也不反复重试）
    VkDeviceMemory   heap_mem;
    void            *heap;
    uint64_t         heap_size;
} vk_ctx_ohos_t;

static vk_ctx_ohos_t g_ctx;

static void set_err(const char *what, VkResult r) {
    snprintf(g_ctx.err, sizeof g_ctx.err, "%s（VkResult=%d）", what, (int)r);
}

// 按需创建。1 = 可用，0 = 不可用（原因在 g_ctx.err）。
static int ensure(void) {
    if (g_ctx.dev) return 1;
    if (g_ctx.tried) return 0;
    g_ctx.tried = 1;
    g_ctx.err[0] = 0;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "dm-ohos";
    app.apiVersion = DM_VK_API;
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ii, NULL, &g_ctx.inst);
    if (r != VK_SUCCESS) { set_err("vkCreateInstance", r); return 0; }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_ctx.inst, &n, NULL);
    if (!n) { snprintf(g_ctx.err, sizeof g_ctx.err, "没有任何物理设备"); return 0; }
    VkPhysicalDevice phy[8] = {0};
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(g_ctx.inst, &n, phy);

    // 取第一个带图形队列的设备（这台手机只有一个；写法保持通用）
    for (uint32_t i = 0; i < n && !g_ctx.phys; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phy[i], &qn, NULL);
        VkQueueFamilyProperties *fams = calloc(qn ? qn : 1, sizeof *fams);
        vkGetPhysicalDeviceQueueFamilyProperties(phy[i], &qn, fams);
        for (uint32_t q = 0; q < qn; q++)
            if (fams[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_ctx.phys = phy[i]; g_ctx.qfam = q; break; }
        free(fams);
    }
    if (!g_ctx.phys) { snprintf(g_ctx.err, sizeof g_ctx.err, "没有带图形队列的设备"); return 0; }

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(g_ctx.phys, &pp);
    snprintf(g_ctx.devname, sizeof g_ctx.devname, "%s", pp.deviceName);

    // 阶段 3 要用：缓冲设备地址（着色器里的 SBUFP/UBOP、池的 GPU 地址反查）、动态渲染。
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_ctx.qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.pNext = &f12;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.pNext = &f13;
    r = vkCreateDevice(g_ctx.phys, &di, NULL, &g_ctx.dev);
    if (r != VK_SUCCESS) { set_err("vkCreateDevice", r); return 0; }

    vkGetDeviceQueue(g_ctx.dev, g_ctx.qfam, 0, &g_ctx.queue);

    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.queueFamilyIndex = g_ctx.qfam; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    r = vkCreateCommandPool(g_ctx.dev, &pi, NULL, &g_ctx.pool);
    if (r != VK_SUCCESS) { set_err("vkCreateCommandPool", r); return 0; }

    LOG("共享 Vulkan 上下文就绪：%s（队列族 %u）", g_ctx.devname, g_ctx.qfam);
    return 1;
}

VkInstance       dm_ohos_vk_instance(void) { ensure(); return g_ctx.inst; }
VkPhysicalDevice dm_ohos_vk_phys(void)     { ensure(); return g_ctx.phys; }
VkDevice         dm_ohos_vk_device(void)   { ensure(); return g_ctx.dev; }
uint32_t         dm_ohos_vk_qfam(void)     { ensure(); return g_ctx.qfam; }
VkQueue          dm_ohos_vk_queue(void)    { ensure(); return g_ctx.queue; }
const char      *dm_ohos_vk_err(void)      { return g_ctx.err; }
const char      *dm_ohos_vk_devname(void)  { return g_ctx.devname; }

int dm_ohos_vk_oneshot(void (*record)(VkCommandBuffer, void *), void *ud) {
    if (!ensure()) return -1;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = g_ctx.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(g_ctx.dev, &ai, &cb) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return -1;
    record(cb, ud);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (vkQueueSubmit(g_ctx.queue, 1, &si, NULL) != VK_SUCCESS) return -1;
    vkQueueWaitIdle(g_ctx.queue);
    vkFreeCommandBuffers(g_ctx.dev, g_ctx.pool, 1, &cb);
    return 0;
}

void *dm_ohos_heap_reserve(uint64_t size) {
    if (g_ctx.heap) return g_ctx.heap;         // 幂等：重复调用返回同一块
    if (!size) return NULL;
    if (!ensure()) return NULL;

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_ctx.phys, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int mt = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & want) == want) { mt = (int)i; break; }
    if (mt < 0) {
        snprintf(g_ctx.err, sizeof g_ctx.err, "没有「CPU 可写 + 显存本地 + 一致」的内存类型");
        return NULL;
    }

    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = size;
    mi.memoryTypeIndex = (uint32_t)mt;
    VkResult r = vkAllocateMemory(g_ctx.dev, &mi, NULL, &g_ctx.heap_mem);
    if (r != VK_SUCCESS) { set_err("vkAllocateMemory(客体堆)", r); return NULL; }

    r = vkMapMemory(g_ctx.dev, g_ctx.heap_mem, 0, VK_WHOLE_SIZE, 0, &g_ctx.heap);
    if (r != VK_SUCCESS) {
        set_err("vkMapMemory(客体堆)", r);
        vkFreeMemory(g_ctx.dev, g_ctx.heap_mem, NULL);
        g_ctx.heap_mem = NULL;
        return NULL;
    }
    g_ctx.heap_size = size;
    LOG("客体堆预留成功：%llu MiB，CPU 映射 %p（内存类型 %d）",
        (unsigned long long)(size >> 20), g_ctx.heap, mt);
    return g_ctx.heap;
}

void dm_ohos_heap_release(void) {
    if (g_ctx.heap) { vkUnmapMemory(g_ctx.dev, g_ctx.heap_mem); g_ctx.heap = NULL; }
    if (g_ctx.heap_mem) { vkFreeMemory(g_ctx.dev, g_ctx.heap_mem, NULL); g_ctx.heap_mem = NULL; }
    g_ctx.heap_size = 0;
}

VkDeviceMemory dm_ohos_heap_memory(void) { return g_ctx.heap_mem; }
void          *dm_ohos_heap_base(void)   { return g_ctx.heap; }
uint64_t       dm_ohos_heap_size(void)   { return g_ctx.heap_size; }
