// 回放层的池模块（第一步）。接口与设计理由见 nvn_vk.h。
// The pool module of the replay layer (step 1); see nvn_vk.h for the interface and the rationale.
#include "nvn_vk.h"

#include "dm_log.h"
#include "vk_ctx_ohos.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// 池登记表。**刻意用"不持有缓冲的指针"那套**：上游 `pool_ent` 存的是不持有的 `id<MTLBuffer>`，
// 缓冲引用另放在 `g_keep` 里；Vulkan 这边 `VkBuffer` 是句柄、由我们自己销毁，所以只需要一张表。
typedef struct {
    uintptr_t lo, hi;      // 池的**页对齐**覆盖区间（上游同样把 lo 向下、hi 向上取整到页）
    uintptr_t base;        // 建缓冲时的起点（= lo）
    VkBuffer  buf;
    uint64_t  gpu;         // 缓冲的设备地址（GPU 地址 = gpu + (addr − base)）
    const void *key;       // 注销时用的键（游戏给的那个 mem 指针）
} vk_pool_ent;

static vk_pool_ent *g_pools;
static int g_npool, g_cap;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_err[256];

const char *nvn_vk_last_error(void) { return g_err; }
int nvn_vk_pool_count(void) { return g_npool; }

static void set_err(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, a);
    va_end(a);
}

int nvn_vk_pool_add(const void *mem, uint64_t size) {
    if (!mem || !size) { set_err("池登记：mem/size 为空"); return -1; }

    void *heap = dm_ohos_heap_base();
    VkDeviceMemory hm = dm_ohos_heap_memory();
    VkDevice dev = dm_ohos_vk_device();
    if (!heap || !hm || !dev) {
        // 客体堆还没预留（hle_svc.c 那边要 DM_VK_HEAP=1）。这条路上没法零拷贝——如实报，别硬凑。
        set_err("客体堆还没预留（dm_ohos_heap_reserve 未成功）—— 池没法建成零拷贝缓冲");
        return -1;
    }

    // 页对齐覆盖区间（照抄上游 mtl_pool_add 的取整方式）
    uintptr_t pg = (uintptr_t)getpagesize();
    uintptr_t lo = (uintptr_t)mem & ~(pg - 1);
    uintptr_t hi = ((uintptr_t)mem + size + pg - 1) & ~(pg - 1);

    uintptr_t hb = (uintptr_t)heap, he = hb + (uintptr_t)dm_ohos_heap_size();
    if (lo < hb || hi > he) {
        set_err("池 %p+%#llx 不在客体堆 [%p, %p) 内 —— 拿不到零拷贝缓冲",
                mem, (unsigned long long)size, (void *)hb, (void *)he);
        return -1;
    }
    uint64_t off = (uint64_t)(lo - hb);

    // 建"整池一个 VkBuffer"，绑定偏移 = 池在堆内的偏移。绑定点/动态偏移由 nvn_vk_addr 给出的 off 决定。
    // One VkBuffer per pool, bound at the pool's offset inside the heap.
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = (VkDeviceSize)(hi - lo);
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    VkResult r = vkCreateBuffer(dev, &bi, NULL, &buf);
    if (r != VK_SUCCESS) { set_err("vkCreateBuffer（池 %p，%llu 字节）失败 VkResult=%d", mem, (unsigned long long)(hi - lo), (int)r); return -1; }

    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    if (off % mr.alignment) {
        set_err("池偏移 %llu 不是 alignment %llu 的整数倍 —— 池起点要页对齐（游戏分配器给的是页对齐）",
                (unsigned long long)off, (unsigned long long)mr.alignment);
        vkDestroyBuffer(dev, buf, NULL);
        return -1;
    }
    // 客体堆用的内存类型必须在这个缓冲的 memoryTypeBits 里，否则绑定非法（先查，别等驱动报）。
    uint32_t hmt = dm_ohos_heap_mem_type();
    if (!(mr.memoryTypeBits & (1u << hmt))) {
        set_err("客体堆的内存类型 %u 不在这个池缓冲的 memoryTypeBits（%#x）里", hmt, mr.memoryTypeBits);
        vkDestroyBuffer(dev, buf, NULL);
        return -1;
    }
    r = vkBindBufferMemory(dev, buf, hm, (VkDeviceSize)off);
    if (r != VK_SUCCESS) { set_err("vkBindBufferMemory（池偏移 %llu）失败 VkResult=%d", (unsigned long long)off, (int)r); vkDestroyBuffer(dev, buf, NULL); return -1; }

    uint64_t gpu = 0;
    PFN_vkGetBufferDeviceAddress pGetBDA =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddress");
    if (pGetBDA) {
        VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        ai.buffer = buf;
        gpu = pGetBDA(dev, &ai);
    }

    pthread_mutex_lock(&g_lock);
    if (g_npool == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_pools = (vk_pool_ent *)realloc(g_pools, (size_t)g_cap * sizeof *g_pools);
    }
    g_pools[g_npool++] = (vk_pool_ent){ lo, hi, lo, buf, gpu, mem };
    pthread_mutex_unlock(&g_lock);

    LOG("池登记：%p+%#llx → 堆偏移 %#llx，VkBuffer %p，设备地址 %#llx",
        mem, (unsigned long long)size, (unsigned long long)off, (void *)buf, (unsigned long long)gpu);
    return 0;
}

void nvn_vk_pool_del(const void *mem) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_npool; i++) {
        if (g_pools[i].key == mem) {
            VkBuffer gone = g_pools[i].buf;
            g_pools[i] = g_pools[--g_npool];
            pthread_mutex_unlock(&g_lock);
            // 上游在这里是"等用过它的提交完成后放掉"（defer_obj_release）。提交侧还没有，先直接销毁；
            // 等有了提交队列再补退役队列（那时池可能还在 GPU 手上）。
            VkDevice dev = dm_ohos_vk_device();
            if (dev && gone) vkDestroyBuffer(dev, gone, NULL);
            return;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

// 地址反查。命中时填 buf/off（以及 gpu，若给了非空指针）。
static int lookup(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *gpu) {
    if (buf) *buf = VK_NULL_HANDLE;
    if (off) *off = 0;
    if (gpu) *gpu = 0;
    if (!addr || !len) return 0;
    int hit = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_npool; i++) {
        if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
            uint64_t o = (uint64_t)(addr - g_pools[i].base);
            if (buf) *buf = g_pools[i].buf;
            if (off) *off = o;
            if (gpu) *gpu = g_pools[i].gpu + o;
            hit = 1;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return hit;
}

int nvn_vk_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off) {
    return lookup(addr, len, buf, off, NULL);
}

int nvn_vk_gpu_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *gpu) {
    return lookup(addr, len, buf, off, gpu);
}
