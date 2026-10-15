// 回放层的池模块（第一步）。接口与设计理由见 nvn_vk.h。
// The pool module of the replay layer (step 1); see nvn_vk.h for the interface and the rationale.
#include "nvn_vk.h"

#include "dm_log.h"
#include "nvn_obj.h"        // 命令记录格式（op_hdr / p_* 载荷）—— 回放层的输入
#include "vk_ctx_ohos.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

// ================================================================================
// 回放层（对标 nvn_mtl.m 的 mtl_exec 那部分）—— 本步只覆盖"最小可出图"的那一小撮命令。
// The replay layer (the mtl_exec counterpart). This step covers only the commands needed to draw.
// ================================================================================

static nvn_vk_shader_fn g_shader_fn;
void nvn_vk_set_shader_resolver(nvn_vk_shader_fn fn) { g_shader_fn = fn; }

static nvn_vk_stats_t g_st;
// 载入字节数单独放：它由"打开缓存"决定，**不能被每帧的 stats_reset 清掉**
// （踩过的坑：原先它在 g_st 里，frame_begin 一 reset 就变 0，判据误报"没载入"）
static uint64_t g_pso_cache_blob;

void nvn_vk_stats_get(nvn_vk_stats_t *out) {
    if (!out) return;
    *out = g_st;
    out->pso_cache_blob = g_pso_cache_blob;
}
void nvn_vk_stats_reset(void) { memset(&g_st, 0, sizeof g_st); }

// ---- 逐通道 GPU 计时（时间戳查询）。设备的 timestampValidBits / timestampPeriod 决定可不可用 ----
#define NVN_VK_TS_MAX 32
#define NVN_VK_TS_PASS 8
static VkCommandBuffer g_cb;          // 本帧正在录制的命令缓冲（回放层与计时都要用）
static VkQueryPool g_qp;
static float       g_ts_period;
static int         g_ts_ok;
static uint32_t    g_ts_next;
static uint32_t    g_ts_frame[2];
static uint32_t    g_ts_pass[NVN_VK_TS_PASS][2];
static uint64_t    g_ts_val[NVN_VK_TS_MAX];
static int         g_pass_cur = -1;

static void ts_init(void) {
    if (g_qp || g_ts_ok < 0) return;
    VkPhysicalDevice pd = dm_ohos_vk_phys();
    VkDevice dev = dm_ohos_vk_device();
    if (!pd || !dev) return;
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    g_ts_period = pp.limits.timestampPeriod;
    // 有效位在**队列族**属性里（不是物理设备属性）：0 = 该队列不支持时间戳
    uint32_t qn = 0, valid = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
    if (qn) {
        VkQueueFamilyProperties *qs = calloc(qn, sizeof *qs);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs);
        uint32_t qf = dm_ohos_vk_qfam();
        valid = qf < qn ? qs[qf].timestampValidBits : 0;
        free(qs);
    }
    if (!pp.limits.timestampComputeAndGraphics || g_ts_period <= 0 || !valid) {
        g_ts_ok = -1;
        LOG("【提示】设备不支持 GPU 时间戳（period=%.3f 有效位=%u），测量骨架里的 GPU 耗时记 0",
            (double)g_ts_period, valid);
        return;
    }
    VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = NVN_VK_TS_MAX;
    if (vkCreateQueryPool(dev, &qi, NULL, &g_qp) != VK_SUCCESS) {
        g_qp = VK_NULL_HANDLE; g_ts_ok = -1;
        LOG("【提示】建时间戳查询池失败，GPU 耗时记 0");
        return;
    }
    g_ts_ok = 1;
    LOG("GPU 计时可用：timestampPeriod=%.3f ns，队列族 %u 有效位 %u",
        (double)g_ts_period, dm_ohos_vk_qfam(), valid);
}
static void ts_mark(VkPipelineStageFlagBits stage, uint32_t *slot) {
    if (!g_ts_ok || g_ts_ok < 0 || g_ts_next >= NVN_VK_TS_MAX) { *slot = (uint32_t)-1; return; }
    *slot = g_ts_next++;
    vkCmdWriteTimestamp(g_cb, stage, g_qp, *slot);
}
static void ts_fetch(void) {
    if (g_ts_ok != 1 || !g_ts_next) return;
    if (vkGetQueryPoolResults(dm_ohos_vk_device(), g_qp, 0, g_ts_next, sizeof g_ts_val, g_ts_val,
                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
        return;
}
static uint64_t ts_delta(uint32_t a, uint32_t b) {
    if (a == (uint32_t)-1 || b == (uint32_t)-1 || b >= g_ts_next) return 0;
    if (g_ts_val[b] < g_ts_val[a]) return 0;
    return (uint64_t)((double)(g_ts_val[b] - g_ts_val[a]) * (double)g_ts_period);
}

// 像素字节数（带宽估算用；只列本步会遇到的格式）
static int vk_bpp(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        default: break;
    }
    return 4;
}

// ---- 顶点属性格式：NVN → VkFormat。映射表照抄上游 vfmt()（nvn_mtl.m:2523）逐项换过来 ----
static VkFormat vk_vfmt(int f) {
    switch (f) {
        case 0x01: return VK_FORMAT_R8_UNORM;            case 0x02: return VK_FORMAT_R8_SNORM;
        case 0x03: return VK_FORMAT_R8_UINT;             case 0x04: return VK_FORMAT_R8_SINT;
        case 0x05: return VK_FORMAT_R16_SFLOAT;          case 0x06: return VK_FORMAT_R16_UNORM;
        case 0x07: return VK_FORMAT_R16_SNORM;           case 0x08: return VK_FORMAT_R16_UINT;
        case 0x09: return VK_FORMAT_R16_SINT;            case 0x0A: return VK_FORMAT_R32_SFLOAT;
        case 0x0B: return VK_FORMAT_R32_UINT;            case 0x0C: return VK_FORMAT_R32_SINT;
        case 0x0D: return VK_FORMAT_R8G8_UNORM;          case 0x0E: return VK_FORMAT_R8G8_SNORM;
        case 0x0F: return VK_FORMAT_R8G8_UINT;           case 0x10: return VK_FORMAT_R8G8_SINT;
        case 0x11: return VK_FORMAT_R16G16_SFLOAT;       case 0x12: return VK_FORMAT_R16G16_UNORM;
        case 0x13: return VK_FORMAT_R16G16_SNORM;        case 0x14: return VK_FORMAT_R16G16_UINT;
        case 0x15: return VK_FORMAT_R16G16_SINT;         case 0x16: return VK_FORMAT_R32G32_SFLOAT;
        case 0x17: return VK_FORMAT_R32G32_UINT;         case 0x18: return VK_FORMAT_R32G32_SINT;
        case 0x19: return VK_FORMAT_R8G8B8_UNORM;        case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
        case 0x1B: return VK_FORMAT_R8G8B8_UINT;         case 0x1C: return VK_FORMAT_R8G8B8_SINT;
        case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
        case 0x1F: return VK_FORMAT_R16G16B16_SNORM;     case 0x20: return VK_FORMAT_R16G16B16_UINT;
        case 0x21: return VK_FORMAT_R16G16B16_SINT;      case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
        case 0x23: return VK_FORMAT_R32G32B32_UINT;      case 0x24: return VK_FORMAT_R32G32B32_SINT;
        case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;      case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
        case 0x27: return VK_FORMAT_R8G8B8A8_UINT;       case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
        case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT; case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
        case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;  case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
        case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;   case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;   case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    }
    return VK_FORMAT_UNDEFINED;
}

// ---- 贴图格式：NVN → VkFormat（本步只列渲染目标要用的几个）----
static VkFormat vk_tfmt(int f) {
    switch (f) {
        case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;         // NVN_FORMAT_RGBA8
        case 0x38: return VK_FORMAT_R8G8B8A8_SRGB;          // NVN_FORMAT_RGBA8_SRGB
        case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;    // NVN_FORMAT_RGBA16F
        case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; // NVN_FORMAT_RGB10A2
    }
    return VK_FORMAT_UNDEFINED;
}

// ---- 混合因子 / 混合运算：照抄上游 bfac() / bop()（nvn_mtl.m:2541）----
static VkBlendFactor vk_bfac(int f) {
    switch (f) {
        case 1: return VK_BLEND_FACTOR_ZERO;                     case 2: return VK_BLEND_FACTOR_ONE;
        case 3: return VK_BLEND_FACTOR_SRC_COLOR;                case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_SRC_ALPHA;                case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_DST_ALPHA;                case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 9: return VK_BLEND_FACTOR_DST_COLOR;                case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;            case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;            case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;        case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;        case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp vk_bop(int e) {
    switch (e) {
        case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 4: return VK_BLEND_OP_MIN;      case 5: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

// ---- 渲染目标：tex_t → VkImage / 通道 / 帧缓冲 ----
//
// **为什么不把图像绑到池内存上**（这点很关键，别改回去）：
//   ① Vulkan 不允许线性资源（VkBuffer）与平铺资源（OPTIMAL 图像）**重叠**占同一段内存
//      （`bufferImageGranularity`），而池的 VkBuffer 覆盖整池、图像必然落在其中 ⇒ 别名冲突。
//   ② 更要紧的是**上游本来就不这么做**：`nvn_mtl.m` 里平铺贴图一律 `newTextureWithDescriptor`
//      自建（自带分配），再上传池里的字节；只有 `NVN_TEXTURE_FLAGS_LINEAR` 的贴图才走
//      `newTextureWithDescriptor:offset:bytesPerRow:` 那种"缓冲 + 偏移"。这也正是上游要统计
//      "贴图上传 / 重传"的原因。所以 Vulkan 侧照抄"自建图像"，既忠实又不踩别名规则。
// Why the image is NOT bound to pool memory: Vulkan forbids a linear buffer and an optimal-tiling image
// from overlapping (bufferImageGranularity), and upstream itself creates tiled textures with their own
// allocation and uploads the pool bytes -- only LINEAR textures go through buffer + offset.
typedef struct {
    const tex_t *t; uint64_t serial;
    VkImage img; VkDeviceMemory mem; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
    uint32_t w, h; VkFormat fmt;
    int ext; VkImageLayout ext_layout;      // 外部目标（出图合流）：只引用，不持有；结束布局由调用方定
} vk_rt_ent;
static vk_rt_ent *g_rts; static int g_nrt, g_rtcap;

// ---- 出图合流：外部渲染目标（见 nvn_vk.h）----
// 为什么需要它：回放层自建的渲染目标结束布局是 TRANSFER_SRC（供读回），而"上屏"要求画进交换链图像、
// 结束布局是 PRESENT_SRC_KHR。与其让回放层去认识交换链，不如让调用方把"画到哪"递进来 ——
// 回放层仍然是"按命令流选目标"，只是遇到登记过的就换成调用方那一份。
typedef struct {
    const tex_t *t;
    VkImage img; VkImageView view; VkRenderPass rp; VkFramebuffer fb;
    uint32_t w, h; VkFormat fmt; VkImageLayout final_layout;
} vk_ext_rt;
static vk_ext_rt g_ext[8]; static int g_next;

static vk_ext_rt *ext_get(const tex_t *t) {
    for (int i = 0; i < g_next; i++) if (g_ext[i].t == t) return &g_ext[i];
    return NULL;
}

int nvn_vk_target_bind_external(const void *tex, VkImage img, VkImageView view, VkRenderPass rp,
                                VkFramebuffer fb, uint32_t w, uint32_t h, VkFormat fmt,
                                VkImageLayout final_layout) {
    if (!tex || !img || !rp || !fb || !w || !h) { set_err("外部渲染目标：参数不全"); return -1; }
    vk_ext_rt *x = ext_get((const tex_t *)tex);
    if (!x) {
        if (g_next == (int)(sizeof g_ext / sizeof *g_ext)) { set_err("外部渲染目标表已满"); return -1; }
        x = &g_ext[g_next++];
        memset(x, 0, sizeof *x);
        x->t = (const tex_t *)tex;
    }
    x->img = img; x->view = view; x->rp = rp; x->fb = fb;
    x->w = w; x->h = h; x->fmt = fmt; x->final_layout = final_layout;
    // 已经建过的同贴图 rt 条目要作废：下一帧换的是另一张交换链图像，旧的不能留着被复用
    for (int i = 0; i < g_nrt; i++) if (g_rts[i].t == (const tex_t *)tex) g_rts[i].t = NULL;
    return 0;
}

void nvn_vk_target_unbind_external(const void *tex) {
    for (int i = 0; i < g_next; i++) if (g_ext[i].t == (const tex_t *)tex) g_ext[i].t = NULL;
    for (int i = 0; i < g_nrt; i++) if (g_rts[i].t == (const tex_t *)tex) g_rts[i].t = NULL;
}

static vk_rt_ent *rt_get(const tex_t *t) {
    for (int i = 0; i < g_nrt; i++) if (g_rts[i].t == t) return &g_rts[i];

    // 登记过外部目标就以它为准（出图合流）——自建那一整段整段跳过
    vk_ext_rt *x = ext_get(t);
    if (x && x->img) {
        vk_rt_ent e; memset(&e, 0, sizeof e);
        e.t = t; e.serial = t->serial; e.w = x->w; e.h = x->h; e.fmt = x->fmt;
        e.img = x->img; e.view = x->view; e.rp = x->rp; e.fb = x->fb;
        e.ext = 1; e.ext_layout = x->final_layout;
        if (g_nrt == g_rtcap) { g_rtcap = g_rtcap ? g_rtcap * 2 : 16; g_rts = realloc(g_rts, (size_t)g_rtcap * sizeof *g_rts); }
        g_rts[g_nrt++] = e;
        LOG("渲染目标（外部/出图）：%ux%u VkFormat %d，结束布局 %d（图像由调用方持有）",
            e.w, e.h, (int)e.fmt, (int)e.ext_layout);
        return &g_rts[g_nrt - 1];
    }

    VkDevice dev = dm_ohos_vk_device();
    if (!dev) { set_err("渲染目标：共享上下文不可用"); return NULL; }
    VkFormat fmt = vk_tfmt(t->d.format);
    if (fmt == VK_FORMAT_UNDEFINED) { set_err("渲染目标格式 %d 未映射", t->d.format); return NULL; }
    if (!t->d.width || !t->d.height) { set_err("渲染目标尺寸为 0"); return NULL; }

    vk_rt_ent e; memset(&e, 0, sizeof e);
    e.t = t; e.serial = t->serial; e.w = t->d.width; e.h = t->d.height; e.fmt = fmt;

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
    ii.extent.width = e.w; ii.extent.height = e.h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(dev, &ii, NULL, &e.img);
    if (r != VK_SUCCESS) { set_err("vkCreateImage（渲染目标）失败 VkResult=%d", (int)r); return NULL; }

    // 自建分配（统一内存类型：CPU 可写 + 显存本地 + 一致），与客体堆是两块独立内存 ⇒ 无别名冲突
    {
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, e.img, &mr);
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mi.allocationSize = mr.size; mi.memoryTypeIndex = dm_ohos_heap_mem_type();
        if (!(mr.memoryTypeBits & (1u << dm_ohos_heap_mem_type()))) {
            set_err("统一内存类型 %u 不在图像 memoryTypeBits（%#x）里", dm_ohos_heap_mem_type(), mr.memoryTypeBits);
            goto fail_img;
        }
        r = vkAllocateMemory(dev, &mi, NULL, &e.mem);
        if (r != VK_SUCCESS) { set_err("vkAllocateMemory（渲染目标 %llu 字节）失败 VkResult=%d",
                                       (unsigned long long)mr.size, (int)r); goto fail_img; }
        r = vkBindImageMemory(dev, e.img, e.mem, 0);
        if (r != VK_SUCCESS) { set_err("vkBindImageMemory（渲染目标）失败 VkResult=%d", (int)r); goto fail_mem; }
    }

    {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = e.img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        r = vkCreateImageView(dev, &vi, NULL, &e.view);
        if (r != VK_SUCCESS) { set_err("vkCreateImageView 失败 VkResult=%d", (int)r); goto fail_mem; }
    }
    {
        // 通道：清屏进附件、不读回旧内容；结束时转成 TRANSFER_SRC，读回 / 出图可以直接拷。
        VkAttachmentDescription att; memset(&att, 0, sizeof att);
        att.format = fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 1; rp.pAttachments = &att;
        rp.subpassCount = 1; rp.pSubpasses = &sp;
        r = vkCreateRenderPass(dev, &rp, NULL, &e.rp);
        if (r != VK_SUCCESS) { set_err("vkCreateRenderPass 失败 VkResult=%d", (int)r); goto fail_view; }
    }
    {
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = e.rp; fi.attachmentCount = 1; fi.pAttachments = &e.view;
        fi.width = e.w; fi.height = e.h; fi.layers = 1;
        r = vkCreateFramebuffer(dev, &fi, NULL, &e.fb);
        if (r != VK_SUCCESS) { set_err("vkCreateFramebuffer 失败 VkResult=%d", (int)r); goto fail_rp; }
    }

    if (g_nrt == g_rtcap) { g_rtcap = g_rtcap ? g_rtcap * 2 : 16; g_rts = realloc(g_rts, (size_t)g_rtcap * sizeof *g_rts); }
    g_rts[g_nrt++] = e;
    LOG("渲染目标：%ux%u 格式 %d → VkFormat %d（自建分配，与客体堆无关）", e.w, e.h, t->d.format, (int)fmt);
    return &g_rts[g_nrt - 1];

fail_rp:   vkDestroyRenderPass(dev, e.rp, NULL);
fail_view: vkDestroyImageView(dev, e.view, NULL);
fail_mem:  vkFreeMemory(dev, e.mem, NULL);
fail_img:  vkDestroyImage(dev, e.img, NULL);
    return NULL;
}

// ---- 管线缓存（对标上游 pso_get：按"程序 + 目标格式 + 混合 / 写掩码 + 顶点布局"缓存）----
// **拓扑也要进键**：管线里的 inputAssembly.topology 是建管线时定死的，不把拓扑算进键，
// 换拓扑就会复用错管线——而且**不报错，只是画错**。（这是 3.2 埋下的缺陷，本步修掉。）
typedef struct {
    const void *vs_ctrl, *fs_ctrl;
    VkFormat cfmt;
    VkPrimitiveTopology topo;
    uint32_t chmask, blend_en, src_rgb, dst_rgb, eq_rgb, src_a, dst_a, eq_a;
    int nattr; uint32_t attr[16];
    int nstream; vs_st stream[16];
} vk_pso_key;
typedef struct { vk_pso_key k; VkPipeline pl; VkPipelineLayout lay; VkRenderPass rp; } vk_pso_ent;
static vk_pso_ent *g_psos; static int g_npso, g_psocap;

// ---- 管线缓存落盘（见 nvn_vk.h 的说明：Vulkan 特有，Metal 做不到运行时积累）----
static char            g_pso_path[1024];
static VkPipelineCache g_pso_cache;
static uint64_t        g_pso_saved_built;   // 上次存盘时的 pso_built（有新管线才存，别每帧写盘）

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int nvn_vk_pso_cache_enable(const char *path) {
    VkDevice dev = dm_ohos_vk_device();
    if (!dev) { set_err("共享上下文不可用：%s", dm_ohos_vk_err()); return -1; }
    if (g_pso_cache) { vkDestroyPipelineCache(dev, g_pso_cache, NULL); g_pso_cache = VK_NULL_HANDLE; }
    g_pso_path[0] = 0;
    if (path && *path) {
        snprintf(g_pso_path, sizeof g_pso_path, "%s", path);
        g_pso_path[sizeof g_pso_path - 1] = 0;
    }
    // 盘上有就载入（热启动），没有就空着建（冷启动）
    void *blob = NULL; size_t n = 0;
    if (g_pso_path[0]) {
        FILE *f = fopen(g_pso_path, "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long sz = ftell(f);
                if (sz > 0 && fseek(f, 0, SEEK_SET) == 0) {
                    blob = malloc((size_t)sz);
                    if (blob && fread(blob, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz;
                    else { free(blob); blob = NULL; }
                }
            }
            fclose(f);
        }
    }
    VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    ci.initialDataSize = n; ci.pInitialData = blob;
    VkResult r = vkCreatePipelineCache(dev, &ci, NULL, &g_pso_cache);
    free(blob);
    if (r != VK_SUCCESS) {
        g_pso_cache = VK_NULL_HANDLE;
        set_err("vkCreatePipelineCache 失败 VkResult=%d", (int)r);
        return -1;
    }
    g_pso_cache_blob = n;
    LOG("管线缓存：%s（%zu 字节）%s", n ? "热启动，已载入" : "冷启动（盘上还没有）", n,
        g_pso_path[0] ? g_pso_path : "（不落盘）");
    return 0;
}

static int pso_cache_save(void) {
    VkDevice dev = dm_ohos_vk_device();
    if (!dev || !g_pso_cache || !g_pso_path[0]) return -1;
    size_t n = 0;
    if (vkGetPipelineCacheData(dev, g_pso_cache, &n, NULL) != VK_SUCCESS || !n) return -1;
    void *blob = malloc(n);
    if (!blob) return -1;
    if (vkGetPipelineCacheData(dev, g_pso_cache, &n, blob) != VK_SUCCESS) { free(blob); return -1; }
    FILE *f = fopen(g_pso_path, "wb");
    int ok = (f && fwrite(blob, 1, n, f) == n);
    if (f) fclose(f);
    free(blob);
    LOG("管线缓存：存盘 %zu 字节 → %s", n, g_pso_path);
    return ok ? 0 : -1;
}

void nvn_vk_pso_local_clear(void) {
    VkDevice dev = dm_ohos_vk_device();
    for (int i = 0; i < g_npso; i++) {
        if (dev && g_psos[i].pl) vkDestroyPipeline(dev, g_psos[i].pl, NULL);
        if (dev && g_psos[i].lay) vkDestroyPipelineLayout(dev, g_psos[i].lay, NULL);
    }
    g_npso = 0;
}

// ---- 回放状态（对标上游的 S）----
static struct {
    int has_vp, has_sc;
    p_viewports vp; p_scissor sc;
    const prog_t *prog;
    int nattr; uint32_t attr[16];
    int nstream; vs_st stream[16];
    NVNbufferRange vbuf[16];
    int has_clear; float clear[4];
    const tex_t *rt;
    uint8_t st[6][0x18];        // kind: 0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
    int has_st[6];
} S;

static int g_pass_open; static vk_rt_ent *g_pass_rt;

static void pass_close(void) {
    if (!g_pass_open) return;
    vkCmdEndRenderPass(g_cb);
    if (g_pass_cur >= 0 && g_pass_cur < NVN_VK_TS_PASS)
        ts_mark(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, &g_ts_pass[g_pass_cur][1]);
    g_pass_open = 0; g_pass_rt = NULL; g_pass_cur = -1;
}
static void pass_open(vk_rt_ent *rt) {
    if (g_pass_open) { if (g_pass_rt == rt) return; pass_close(); }
    VkClearValue cv; memset(&cv, 0, sizeof cv);
    if (S.has_clear) memcpy(cv.color.float32, S.clear, sizeof cv.color.float32);
    else cv.color.float32[3] = 1.f;
    // 通道计时：起止都写在通道**外面**（通道内的查询在部分驱动上不准）
    g_pass_cur = (int)g_st.passes;
    if (g_pass_cur < NVN_VK_TS_PASS)
        ts_mark(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, &g_ts_pass[g_pass_cur][0]);
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = rt->rp; bi.framebuffer = rt->fb;
    bi.renderArea.extent.width = rt->w; bi.renderArea.extent.height = rt->h;
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    // 诊断（3.4-D 查"合流后屏幕全黑"）：把**实际用的目标**打出来 —— 运行期证实通道/帧缓冲就是我递进去那份
    LOG("通道打开：%s目标 %ux%u，rp=%p fb=%p img=%p",
        rt->ext ? "**外部/出图**" : "自建", rt->w, rt->h, (void *)rt->rp, (void *)rt->fb, (void *)rt->img);
    vkCmdBeginRenderPass(g_cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    g_pass_open = 1; g_pass_rt = rt;
    g_st.passes++;
    // 带宽估算：本步的通道一律 loadOp=CLEAR（不读入）、storeOp=STORE（写出）
    g_st.bw_store += (uint64_t)rt->w * (uint64_t)rt->h * (uint64_t)vk_bpp(rt->fmt);
}

static VkPipeline pso_get(vk_rt_ent *rt, VkPrimitiveTopology topo) {
    vk_pso_key k; memset(&k, 0, sizeof k);
    k.cfmt = rt->fmt;
    k.topo = topo;
    k.nattr = S.nattr; memcpy(k.attr, S.attr, sizeof k.attr);
    k.nstream = S.nstream; memcpy(k.stream, S.stream, sizeof k.stream);
    if (S.prog && S.prog->nstage > 0) k.vs_ctrl = S.prog->sd[0].control;
    if (S.prog && S.prog->nstage > 1) k.fs_ctrl = S.prog->sd[1].control;
    if (S.has_st[2]) k.chmask = ((const chmask_st *)S.st[2])->mask;
    if (S.has_st[0]) {
        const blend_st *b = (const blend_st *)S.st[0];
        k.src_rgb = b->src_rgb; k.dst_rgb = b->dst_rgb; k.eq_rgb = b->eq_rgb;
        k.src_a = b->src_a; k.dst_a = b->dst_a; k.eq_a = b->eq_a;
    }
    if (S.has_st[1]) k.blend_en = ((const color_st *)S.st[1])->blend_en;

    for (int i = 0; i < g_npso; i++)
        if (!memcmp(&g_psos[i].k, &k, sizeof k) && g_psos[i].rp == rt->rp) { g_st.pso_hit++; return g_psos[i].pl; }

    // 建：先解析两条着色器（走接缝），再按当前状态建管线
    if (!g_shader_fn) { set_err("没有装着色器解析回调（nvn_vk_set_shader_resolver）"); return VK_NULL_HANDLE; }
    nvn_vk_shader vs = {0}, fs = {0};
    if (!k.vs_ctrl || !g_shader_fn(k.vs_ctrl, 0, &vs)) { set_err("顶点着色器解析失败（控制段 %p）", k.vs_ctrl); return VK_NULL_HANDLE; }
    if (!k.fs_ctrl || !g_shader_fn(k.fs_ctrl, 1, &fs)) { set_err("片元着色器解析失败（控制段 %p）", k.fs_ctrl); return VK_NULL_HANDLE; }

    VkDevice dev = dm_ohos_vk_device();
    uint64_t t_build = now_ns();     // 建管线的耗时（"落盘缓存"的判据就靠它）
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    VkPipelineLayout lay = VK_NULL_HANDLE;
    VkPipeline pl = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)vs.nwords * 4; mi.pCode = vs.spirv;
    if (vkCreateShaderModule(dev, &mi, NULL, &vm) != VK_SUCCESS) { set_err("vkCreateShaderModule(顶点) 失败"); return VK_NULL_HANDLE; }
    mi.codeSize = (size_t)fs.nwords * 4; mi.pCode = fs.spirv;
    if (vkCreateShaderModule(dev, &mi, NULL, &fm) != VK_SUCCESS) { set_err("vkCreateShaderModule(片元) 失败"); vkDestroyShaderModule(dev, vm, NULL); return VK_NULL_HANDLE; }

    VkVertexInputBindingDescription binds[16]; memset(binds, 0, sizeof binds);
    for (int i = 0; i < k.nstream && i < 16; i++) {
        binds[i].binding = (uint32_t)i; binds[i].stride = k.stream[i].stride;
        binds[i].inputRate = k.stream[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }
    VkVertexInputAttributeDescription attrs[16]; memset(attrs, 0, sizeof attrs);
    int nattr_ok = 0;
    for (int a = 0; a < k.nattr && a < 16; a++) {
        VkFormat af = vk_vfmt(k.attr[a] >> 0 & 0xFF);
        if (af == VK_FORMAT_UNDEFINED) {
            set_err("顶点属性 %d 的格式 %#x 未映射（上游同样会在此报红）", a, k.attr[a] & 0xFF);
            goto fail_mod;
        }
        attrs[nattr_ok].location = (uint32_t)a;
        attrs[nattr_ok].binding = (k.attr[a] >> 27) & 0x1F;      // va_st.stream
        attrs[nattr_ok].format = af;
        attrs[nattr_ok].offset = (k.attr[a] >> 8) & 0x7FFFF;     // va_st.offset
        nattr_ok++;
    }

    {
        VkPipelineShaderStageCreateInfo st[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL },
        };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = (uint32_t)k.nstream;
        vi.pVertexBindingDescriptions = binds;
        vi.vertexAttributeDescriptionCount = (uint32_t)nattr_ok;
        vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = k.topo;                                    // 拓扑已进键（见 vk_pso_key 的说明）
        VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // ⚠️ 不留 NULL：规范允许"无深度附件时 pDepthStencilState 为 NULL"，但真机上驱动崩过（阶段 2 实验 A）。
        VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.stencilTestEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState att = { 0 };
        att.blendEnable = k.blend_en ? VK_TRUE : VK_FALSE;
        att.srcColorBlendFactor = vk_bfac((int)k.src_rgb); att.dstColorBlendFactor = vk_bfac((int)k.dst_rgb);
        att.colorBlendOp = vk_bop((int)k.eq_rgb);
        att.srcAlphaBlendFactor = vk_bfac((int)k.src_a);  att.dstAlphaBlendFactor = vk_bfac((int)k.dst_a);
        att.alphaBlendOp = vk_bop((int)k.eq_a);
        att.colorWriteMask = k.chmask ? (VkColorComponentFlags)k.chmask
                                      : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &att;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        if (vkCreatePipelineLayout(dev, &li, NULL, &lay) != VK_SUCCESS) { set_err("vkCreatePipelineLayout 失败"); goto fail_mod; }
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gi.stageCount = 2; gi.pStages = st;
        gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vps; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &dss; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
        gi.layout = lay; gi.renderPass = rt->rp;
        VkResult r = vkCreateGraphicsPipelines(dev, g_pso_cache, 1, &gi, NULL, &pl);
        if (r != VK_SUCCESS) { set_err("vkCreateGraphicsPipelines 失败 VkResult=%d", (int)r); goto fail_lay; }
    }

    if (g_npso == g_psocap) { g_psocap = g_psocap ? g_psocap * 2 : 64; g_psos = realloc(g_psos, (size_t)g_psocap * sizeof *g_psos); }
    g_psos[g_npso++] = (vk_pso_ent){ k, pl, lay, rt->rp };
    g_st.pso_built++;
    g_st.pso_build_ns += now_ns() - t_build;
    vkDestroyShaderModule(dev, vm, NULL);
    vkDestroyShaderModule(dev, fm, NULL);
    return pl;

fail_lay: vkDestroyPipelineLayout(dev, lay, NULL);
fail_mod: vkDestroyShaderModule(dev, fm, NULL); vkDestroyShaderModule(dev, vm, NULL);
    return VK_NULL_HANDLE;
}

// 图元类型映射（照抄上游 nvn_mtl.m:4662 的 switch）
static int vk_topo(int prim, VkPrimitiveTopology *out) {
    switch (prim) {
        case 0: *out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return 1;
        case 1: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return 1;
        case 3: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return 1;
        case 4: case 7: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return 1;
        case 5: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return 1;
    }
    return 0;
}

// 绘制前的公共部分：查目标 → 查/建管线 → 开通道 → 绑管线 → 动态视口/剪裁 → 绑顶点缓冲。
// 返回管线（VK_NULL_HANDLE = 这次绘制跳过），目标写到 *rt_out。
static VkPipeline draw_begin(int prim, vk_rt_ent **rt_out) {
    *rt_out = NULL;
    if (!S.prog || !S.rt) { g_st.ops_skipped++; return VK_NULL_HANDLE; }
    VkPrimitiveTopology topo;
    if (!vk_topo(prim, &topo)) { set_err("图元类型 %d 未实现（上游同样报红）", prim); g_st.ops_skipped++; return VK_NULL_HANDLE; }
    vk_rt_ent *rt = rt_get(S.rt);
    if (!rt) { g_st.ops_skipped++; return VK_NULL_HANDLE; }
    VkPipeline pl = pso_get(rt, topo);
    if (!pl) { g_st.ops_skipped++; return VK_NULL_HANDLE; }

    pass_open(rt);
    vkCmdBindPipeline(g_cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);

    VkViewport vp = { 0, 0, (float)rt->w, (float)rt->h, 0.f, 1.f };
    if (S.has_vp) {
        vp.x = S.vp.v[0][0]; vp.y = S.vp.v[0][1];
        vp.width = S.vp.v[0][2]; vp.height = S.vp.v[0][3];
        vp.minDepth = S.vp.v[0][4]; vp.maxDepth = S.vp.v[0][5];
    }
    vkCmdSetViewport(g_cb, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { rt->w, rt->h } };
    if (S.has_sc) {
        sc.offset.x = S.sc.x; sc.offset.y = S.sc.y;
        sc.extent.width = (uint32_t)S.sc.w; sc.extent.height = (uint32_t)S.sc.h;
    }
    vkCmdSetScissor(g_cb, 0, 1, &sc);

    // 顶点缓冲：地址经池反查（3.1-C 那套）——这就是"游戏地址 → GPU 缓冲 + 偏移"的实际用法
    for (int i = 0; i < S.nstream && i < 16; i++) {
        if (!S.vbuf[i].address) continue;
        VkBuffer vb = VK_NULL_HANDLE; uint64_t off = 0;
        if (!nvn_vk_addr((uint64_t)S.vbuf[i].address, 1, &vb, &off)) {
            set_err("顶点流 %d 的地址 %#llx 不在任何已登记的池里", i, (unsigned long long)S.vbuf[i].address);
            g_st.ops_skipped++; return VK_NULL_HANDLE;
        }
        VkDeviceSize o = (VkDeviceSize)off;
        vkCmdBindVertexBuffers(g_cb, (uint32_t)i, 1, &vb, &o);
    }
    *rt_out = rt;
    return pl;
}

// 非索引绘制（对标上游的 draw()）
static void draw(const p_draw *d) {
    if (!d->count) return;
    vk_rt_ent *rt = NULL;
    if (!draw_begin(d->prim, &rt)) return;
    vkCmdDraw(g_cb, (uint32_t)d->count, 1, (uint32_t)d->first, 0);
    g_st.draws++;
}

// 索引绘制（对标上游 draw(dw, di) 的索引分支）：索引缓冲同样走池反查
static void draw_idx(const p_draw_idx *d) {
    if (!d->count) return;
    vk_rt_ent *rt = NULL;
    if (!draw_begin(d->prim, &rt)) return;
    VkBuffer ib = VK_NULL_HANDLE; uint64_t off = 0;
    if (!nvn_vk_addr((uint64_t)d->ibuf, 1, &ib, &off)) {
        set_err("索引缓冲地址 %#llx 不在任何已登记的池里", (unsigned long long)d->ibuf);
        g_st.ops_skipped++; return;
    }
    VkIndexType it = d->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    uint64_t need = it == VK_INDEX_TYPE_UINT16 ? 2 : 4;
    if (off % need) {
        set_err("索引缓冲偏移 %llu 不是索引宽度 %llu 的整数倍",
                (unsigned long long)off, (unsigned long long)need);
        g_st.ops_skipped++; return;
    }
    vkCmdBindIndexBuffer(g_cb, ib, (VkDeviceSize)off, it);
    vkCmdDrawIndexed(g_cb, (uint32_t)d->count,
                     d->instances > 0 ? (uint32_t)d->instances : 1,
                     0, (int32_t)d->base_vertex, (uint32_t)d->base_instance);
    g_st.draws++;
}

int nvn_vk_frame_begin(void) {
    if (g_cb) { set_err("已经有一帧在录制"); return -1; }
    VkDevice dev = dm_ohos_vk_device();
    VkCommandPool pool = dm_ohos_vk_pool();
    if (!dev || !pool) { set_err("共享上下文不可用：%s", dm_ohos_vk_err()); return -1; }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &g_cb) != VK_SUCCESS) { set_err("vkAllocateCommandBuffers 失败"); return -1; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cb, &bi) != VK_SUCCESS) { set_err("vkBeginCommandBuffer 失败"); return -1; }
    g_pass_open = 0; g_pass_rt = NULL; g_pass_cur = -1;
    nvn_vk_stats_reset();
    // 帧计时：起止各一个时间戳
    ts_init();
    if (g_ts_ok == 1) vkCmdResetQueryPool(g_cb, g_qp, 0, NVN_VK_TS_MAX);
    g_ts_next = 0;
    ts_mark(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, &g_ts_frame[0]);
    return 0;
}

int nvn_vk_frame_end(void) {
    if (!g_cb) { set_err("没有在录制的帧"); return -1; }
    VkDevice dev = dm_ohos_vk_device();
    pass_close();
    ts_mark(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, &g_ts_frame[1]);
    if (vkEndCommandBuffer(g_cb) != VK_SUCCESS) { set_err("vkEndCommandBuffer 失败"); goto fail; }
    {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &g_cb;
        VkQueue q = dm_ohos_vk_queue();
        if (vkQueueSubmit(q, 1, &si, NULL) != VK_SUCCESS) { set_err("vkQueueSubmit 失败"); goto fail; }
        vkQueueWaitIdle(q);
    }
    // 取时间戳：整帧 + 逐通道
    ts_fetch();
    g_st.gpu_ns = ts_delta(g_ts_frame[0], g_ts_frame[1]);
    g_st.pass_n = g_st.passes < NVN_VK_TS_PASS ? g_st.passes : NVN_VK_TS_PASS;
    for (uint64_t i = 0; i < g_st.pass_n; i++)
        g_st.pass_gpu_ns[i] = ts_delta(g_ts_pass[i][0], g_ts_pass[i][1]);
    vkFreeCommandBuffers(dev, dm_ohos_vk_pool(), 1, &g_cb);
    g_cb = NULL;
    // 有新管线才存盘（别每帧写盘）
    if (g_st.pso_built != g_pso_saved_built) {
        g_pso_saved_built = g_st.pso_built;
        pso_cache_save();
    }
    return 0;
fail:
    vkFreeCommandBuffers(dev, dm_ohos_vk_pool(), 1, &g_cb);
    g_cb = NULL;
    return -1;
}

void nvn_vk_exec(const uint8_t *handle) {
    static int depth;
    if (!handle || depth > 8) return;
    depth++;
    const uint8_t *p = handle;
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        const void *a = p + sizeof h;
        g_st.ops++;
        switch (h.op) {
            case OP_END: depth--; return;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); nvn_vk_exec((const uint8_t *)t); break; }
            case OP_SET_RT: {
                const p_set_rt *r = (const p_set_rt *)a;
                const tex_t *nt = r->n > 0 ? r->color[0] : NULL;
                if (nt != S.rt) { pass_close(); S.rt = nt; }
                break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = (const p_clear_color *)a;
                S.has_clear = 1; memcpy(S.clear, c->rgba, sizeof S.clear);
                break;
            }
            case OP_VIEWPORTS: S.vp = *(const p_viewports *)a; S.has_vp = 1; break;
            case OP_SCISSOR:   S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
            case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_STATE: {
                const p_bind_state *b = (const p_bind_state *)a;
                if (b->kind >= 0 && b->kind < 6) { memcpy(S.st[b->kind], b->bytes, sizeof S.st[0]); S.has_st[b->kind] = 1; }
                break;
            }
            case OP_BIND_VATTR: {
                // 上游允许从任意槽位开始批量绑定（first != 0）——早先只认 first == 0，
                // 非 0 起点会被**静默忽略**（又是一类"不报错的错"），这里补上。
                const p_bind_vattr *b = (const p_bind_vattr *)a;
                if (b->first >= 0 && b->n >= 0 && b->first + b->n <= 16) {
                    for (int i = 0; i < b->n; i++) S.attr[b->first + i] = b->st[i];
                    if (b->first + b->n > S.nattr) S.nattr = b->first + b->n;
                } else { set_err("顶点属性槽位越界 first=%d n=%d", b->first, b->n); g_st.ops_skipped++; }
                break;
            }
            case OP_BIND_VSTREAM: {
                const p_bind_vstream *b = (const p_bind_vstream *)a;
                if (b->first >= 0 && b->n >= 0 && b->first + b->n <= 16) {
                    for (int i = 0; i < b->n; i++) S.stream[b->first + i] = b->st[i];
                    if (b->first + b->n > S.nstream) S.nstream = b->first + b->n;
                } else { set_err("顶点流槽位越界 first=%d n=%d", b->first, b->n); g_st.ops_skipped++; }
                break;
            }
            case OP_BIND_VBUF: {
                const p_bind_vbuf *b = (const p_bind_vbuf *)a;
                for (int i = 0; i < b->n && b->first + i < 16; i++) S.vbuf[b->first + i] = b->r[i];
                break;
            }
            case OP_DRAW: draw((const p_draw *)a); break;
            case OP_DRAW_IDX: draw_idx((const p_draw_idx *)a); break;
            default: g_st.ops_skipped++; break;   // 本步不处理的命令：计数，不静默
        }
        p += h.size;
    }
}

VkImage nvn_vk_target_image(void) { return (S.rt && g_nrt) ? g_rts[g_nrt - 1].img : VK_NULL_HANDLE; }
int nvn_vk_target_extent(uint32_t *w, uint32_t *h) {
    if (!g_nrt) return -1;
    if (w) *w = g_rts[g_nrt - 1].w;
    if (h) *h = g_rts[g_nrt - 1].h;
    return 0;
}
VkImageLayout nvn_vk_target_layout(void) {
    // 外部目标（出图合流）的结束布局由调用方的通道决定（上屏是 PRESENT_SRC_KHR）；自建的一律 TRANSFER_SRC
    if (S.rt && g_nrt) { vk_rt_ent *e = &g_rts[g_nrt - 1]; if (e->ext) return e->ext_layout; }
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}
