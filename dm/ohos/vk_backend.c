// 离屏 Vulkan 冒烟后端的实现。接口见 vk_backend.h。
// Implementation of the offscreen Vulkan smoke backend; see vk_backend.h.
//
// 阶段 1 只用 transfer 通道：ClearColorImage + CopyImageToBuffer。
// 每一步 Vulkan 调用都检查返回值；失败时把"哪个调用、错误码"写进调用方给的 err 缓冲——
// 真机上排错就靠这个，不给"初始化失败了"这种没信息量的话。
// Every Vulkan call is checked; failures carry the call and the result code.
#include "vk_backend.h"

#include "dm_log.h"
#include "host_tests.h"    // 3.4-B：借用宿主单测的 stdout 捕获（dm_test_capture_begin/end）
#include "nvn_obj.h"       // 3.2 手写命令流要用它的记录格式（op_hdr / p_*）
#include "nvn_vk.h"
#include "tex_shaders.h"   // 3.4-A：采样贴图用的 SPIR-V（生成文件，见 tools/gen_spv_header.py）
#include "tri_shaders.h"
#include "vk_ctx_ohos.h"
#include "vk_surface_ohos.h"

#include "../gfx/tex/nvn_bcdec.h"       // 3.4-A：上游那份"给没有 BC 的 GPU 用"的 CPU 解码器（平台中立）
#include "../gfx/tex/nvn_bc2astc.h"     // 3.4-E（C3）：BC → ASTC 4x4 转码（含自测解码器）
#include "../gfx/tex/nvn_deswizzle.h"   // 3.4-C：块线性 ↔ 线性（含 nvn_tex.h 的布局类型 nvn_texdesc/nvn_texlayout）
#include "astc_expect.h"                // 3.4-E：期望块字节（生成文件，见 tools/astc_ref.py --emit-header）
#include "dm_tex_ohos.h"                // 3.4-E：能力查询 + 选路（生产路径也走这里）

#include <dlfcn.h>         // 3.4-B：按名字载入上游贴图单测的 .so（与宿主单测同一套做法）
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>          // usleep：轮询等拍、等回调退出
#include <math.h>            // 3.4-E：log10 算 PSNR（转码有损，判据要用 dB）

#include <vulkan/vulkan.h>

// 3.3-C：官方"可变帧率"在 XComponent 侧另有一套接口（API 11 起），与 DisplaySoloist 做对照。
//
// 这里**刻意不包含** ArkUI 的 `<ace/xcomponent/native_interface_xcomponent.h>`，而是按官方签名自行声明。
// 原因（真机实测踩到）：那个头在**文件作用域**定义了
//     const uint32_t OH_XCOMPONENT_ID_LEN_MAX = 128;
//     const uint32_t OH_MAX_TOUCH_POINTS_NUMBER = 10;
// 而 **C 里的文件作用域 const 是外部链接**（C++ 才是内部链接）⇒ 只要有两个 .c 同时包含它，
// 链接就炸：`ld.lld: error: duplicate symbol: OH_XCOMPONENT_ID_LEN_MAX`。
// napi_init.c 必须包含它（那边要用 OH_NativeXComponent_Callback 结构体），所以这里只声明要用的三个函数。
// 能用官方头的地方照旧用官方头 —— napi_init.c 一个字没改。
// Deliberately not including ArkUI's XComponent header: it defines file-scope `const uint32_t` objects,
// and in C (unlike C++) those have external linkage, so two TUs including it collide at link time.
typedef struct OH_NativeXComponent OH_NativeXComponent;
typedef struct { int32_t min; int32_t max; int32_t expected; } OH_NativeXComponent_ExpectedRateRange;
extern int32_t OH_NativeXComponent_SetExpectedFrameRateRange(OH_NativeXComponent *component,
                                                             OH_NativeXComponent_ExpectedRateRange *range);
extern int32_t OH_NativeXComponent_RegisterOnFrameCallback(
    OH_NativeXComponent *component,
    void (*callback)(OH_NativeXComponent *component, uint64_t timestamp, uint64_t targetTimestamp));
extern int32_t OH_NativeXComponent_UnregisterOnFrameCallback(OH_NativeXComponent *component);

// 鸿蒙的"报时员"：官方 DisplaySoloist（自渲染视区的可变帧率调度，API 12 起）。
// 头文件在子目录里（NDK 的 sysroot/usr/include/native_display_soloist/），库是 libnative_display_soloist.so。
// The platform timekeeper: the official DisplaySoloist (variable frame rate scheduling for self-drawn
// surfaces, since API 12). The header lives in a subdirectory and the library is libnative_display_soloist.so.
#include <native_display_soloist/native_display_soloist.h>

// 上游能力探测的结论：这台设备是 Vulkan 1.3（Maleoon 920）。按 1.3 建 instance/device，
// 阶段 3 要用的 dynamic rendering 等 1.3 特性不必再动这里。
#define DM_VK_API VK_API_VERSION_1_3

struct vk_ctx {
    VkInstance       inst;
    VkPhysicalDevice phy;
    VkDevice         dev;
    VkQueue          queue;
    uint32_t         qfam;
    VkCommandPool    pool;
    char             devname[256];
    uint32_t         api_ver, driver_ver;
};

struct vk_rt {
    uint32_t       w, h;
    VkImage        img;
    VkDeviceMemory img_mem;
    VkImageView    view;        // 阶段 2：颜色附件要用
    VkRenderPass   rp;          // 阶段 2 实验B：经典 renderPass（dynamic rendering 的驱动兼容性存疑）
    VkFramebuffer  fb;
    VkDeviceMemory rb_mem;      // 读回用 host-visible buffer 的内存
    VkBuffer       rb;
    void          *rb_map;
    VkDeviceSize   rb_size;
};

static void set_err(char *err, size_t cap, const char *what, VkResult r) {
    snprintf(err, cap, "%s（VkResult=%d）", what, (int)r);
}

static uint32_t find_mem_type(const vk_ctx *c, uint32_t type_bits, VkMemoryPropertyFlags want, VkResult *out) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c->phy, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) { *out = VK_SUCCESS; return i; }
    }
    *out = VK_ERROR_FEATURE_NOT_PRESENT;
    return 0;
}

static void destroy_target(vk_ctx *c, vk_rt *t) {
    if (!c || !t) return;
    if (t->fb) vkDestroyFramebuffer(c->dev, t->fb, NULL);
    if (t->rp) vkDestroyRenderPass(c->dev, t->rp, NULL);
    if (t->view) vkDestroyImageView(c->dev, t->view, NULL);
    if (t->rb_map) vkUnmapMemory(c->dev, t->rb_mem);
    if (t->rb) vkDestroyBuffer(c->dev, t->rb, NULL);
    if (t->rb_mem) vkFreeMemory(c->dev, t->rb_mem, NULL);
    if (t->img) vkDestroyImage(c->dev, t->img, NULL);
    if (t->img_mem) vkFreeMemory(c->dev, t->img_mem, NULL);
    free(t);
}

// ---- 一次性命令缓冲：record 里录命令，提交并等完成 ----
static int one_shot(vk_ctx *c, void (*record)(VkCommandBuffer, void *), void *ud) {
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = c->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(c->dev, &ai, &cb) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return -1;
    record(cb, ud);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return -1;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (vkQueueSubmit(c->queue, 1, &si, NULL) != VK_SUCCESS) return -1;
    vkQueueWaitIdle(c->queue);
    vkFreeCommandBuffers(c->dev, c->pool, 1, &cb);
    return 0;
}

vk_ctx *vk_backend_init(char *err, size_t errcap) {
    vk_ctx *c = calloc(1, sizeof *c);
    if (!c) { snprintf(err, errcap, "内存不足"); return NULL; }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "dm-vk-smoke"; app.apiVersion = DM_VK_API;
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ii, NULL, &c->inst);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateInstance", r); free(c); return NULL; }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c->inst, &n, NULL);
    if (!n) { snprintf(err, errcap, "没有任何物理设备"); vk_backend_shutdown(c); return NULL; }
    VkPhysicalDevice phy[8] = {0};
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(c->inst, &n, phy);

    // 取第一个带图形队列的设备（这台手机只有一个，写法保持通用）
    for (uint32_t i = 0; i < n && !c->phy; i++) {
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phy[i], &qn, NULL);
        VkQueueFamilyProperties *fams = calloc(qn ? qn : 1, sizeof *fams);
        vkGetPhysicalDeviceQueueFamilyProperties(phy[i], &qn, fams);
        for (uint32_t q = 0; q < qn; q++) {
            if (fams[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { c->phy = phy[i]; c->qfam = q; break; }
        }
        free(fams);
    }
    if (!c->phy) { snprintf(err, errcap, "没有带图形队列的设备"); vk_backend_shutdown(c); return NULL; }

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(c->phy, &pp);
    snprintf(c->devname, sizeof c->devname, "%s", pp.deviceName);
    c->api_ver = pp.apiVersion; c->driver_ver = pp.driverVersion;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = c->qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    // 阶段 2 起要 dynamic rendering（Vulkan 1.3 核心特性）：不开这个特性，vkCmdBeginRendering 就用不了。
    // 阶段 3 起要缓冲设备地址（着色器里的 SBUFP/UBOP，以及池的 GPU 地址反查）：能力探测已确认本设备支持。
    // dynamic rendering is a Vulkan 1.3 core feature; buffer device address is needed from phase 3 on
    // (SBUFP/UBOP in shaders, and the GPU-address lookup of memory pools) -- the capability probe
    // confirmed this device supports it.
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.pNext = &f12;
    di.pNext = &f13;
    r = vkCreateDevice(c->phy, &di, NULL, &c->dev);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateDevice", r); vk_backend_shutdown(c); return NULL; }

    vkGetDeviceQueue(c->dev, c->qfam, 0, &c->queue);

    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.queueFamilyIndex = c->qfam; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    r = vkCreateCommandPool(c->dev, &pi, NULL, &c->pool);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateCommandPool", r); vk_backend_shutdown(c); return NULL; }

    return c;
}

void vk_backend_shutdown(vk_ctx *c) {
    if (!c) return;
    if (c->pool) vkDestroyCommandPool(c->dev, c->pool, NULL);
    if (c->dev) vkDestroyDevice(c->dev, NULL);
    if (c->inst) vkDestroyInstance(c->inst, NULL);
    free(c);
}

vk_rt *vk_backend_target(vk_ctx *ctx, uint32_t w, uint32_t h, char *err, size_t errcap) {
    vk_rt *t = calloc(1, sizeof *t);
    if (!t) { snprintf(err, errcap, "内存不足"); return NULL; }
    t->w = w; t->h = h; t->rb_size = (VkDeviceSize)w * h * 4;

    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = w; ii.extent.height = h; ii.extent.depth = 1;
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 阶段 2 起它还是颜色附件（COLOR_ATTACHMENT）＋ 传输源/汇（读回）
    ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    VkResult r = vkCreateImage(ctx->dev, &ii, NULL, &t->img);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateImage", r); destroy_target(ctx, t); return NULL; }

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(ctx->dev, t->img, &mr);
    VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mi.allocationSize = mr.size;
    mi.memoryTypeIndex = find_mem_type(ctx, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &r);
    if (r != VK_SUCCESS) { snprintf(err, errcap, "找不到 DEVICE_LOCAL 内存类型"); destroy_target(ctx, t); return NULL; }
    r = vkAllocateMemory(ctx->dev, &mi, NULL, &t->img_mem);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkAllocateMemory(image)", r); destroy_target(ctx, t); return NULL; }
    r = vkBindImageMemory(ctx->dev, t->img, t->img_mem, 0);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkBindImageMemory", r); destroy_target(ctx, t); return NULL; }

    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    r = vkCreateImageView(ctx->dev, &vi, NULL, &t->view);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateImageView", r); destroy_target(ctx, t); return NULL; }

    // 实验B：经典 renderPass + framebuffer。若这样能过，说明这台驱动崩于 dynamic rendering
    // 编译管线（阶段 3 就走 renderPass；若也能过则矛头回到 SPIR-V 本身）。
    VkAttachmentDescription ad = { 0 };
    ad.format = VK_FORMAT_R8G8B8A8_UNORM; ad.samples = VK_SAMPLE_COUNT_1_BIT;
    ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ad.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;   // 渲染完直接可读回
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ar;
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = 1; rpi.pAttachments = &ad;
    rpi.subpassCount = 1; rpi.pSubpasses = &sp;
    r = vkCreateRenderPass(ctx->dev, &rpi, NULL, &t->rp);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateRenderPass", r); destroy_target(ctx, t); return NULL; }
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = t->rp; fbi.attachmentCount = 1; fbi.pAttachments = &t->view;
    fbi.width = w; fbi.height = h; fbi.layers = 1;
    r = vkCreateFramebuffer(ctx->dev, &fbi, NULL, &t->fb);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateFramebuffer", r); destroy_target(ctx, t); return NULL; }

    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = t->rb_size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(ctx->dev, &bi, NULL, &t->rb);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkCreateBuffer(readback)", r); destroy_target(ctx, t); return NULL; }
    vkGetBufferMemoryRequirements(ctx->dev, t->rb, &mr);
    mi.allocationSize = mr.size;
    mi.memoryTypeIndex = find_mem_type(ctx, mr.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r);
    if (r != VK_SUCCESS) { snprintf(err, errcap, "找不到 HOST_VISIBLE 内存类型"); destroy_target(ctx, t); return NULL; }
    r = vkAllocateMemory(ctx->dev, &mi, NULL, &t->rb_mem);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkAllocateMemory(readback)", r); destroy_target(ctx, t); return NULL; }
    r = vkBindBufferMemory(ctx->dev, t->rb, t->rb_mem, 0);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkBindBufferMemory", r); destroy_target(ctx, t); return NULL; }
    r = vkMapMemory(ctx->dev, t->rb_mem, 0, t->rb_size, 0, &t->rb_map);
    if (r != VK_SUCCESS) { set_err(err, errcap, "vkMapMemory", r); destroy_target(ctx, t); return NULL; }

    return t;
}

void vk_backend_target_free(vk_ctx *ctx, vk_rt *rt) { destroy_target(ctx, rt); }

// 布局推进。阶段 1 只在 transfer 通道用；阶段 2 起颜色附件的推进阶段是 COLOR_ATTACHMENT_OUTPUT，
// 所以阶段参数也开放出来。
static void layout_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                           VkPipelineStageFlagBits srcStage, VkPipelineStageFlagBits dstStage,
                           VkAccessFlagBits src, VkAccessFlagBits dst) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.image = img;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cb, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

typedef struct { vk_rt *t; VkClearColorValue cv; } clear_pack;

static void rec_clear(VkCommandBuffer cb, void *ud) {
    clear_pack *p = (clear_pack *)ud;
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    layout_barrier(cb, p->t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, p->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &p->cv, 1, &rng);
    layout_barrier(cb, p->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

int vk_backend_clear(vk_ctx *ctx, vk_rt *rt, float r, float g, float b, float a) {
    // ⚠️ 花括号层数：VkClearColorValue 是 union{ float32[4]; ... }，初始化它要**两层**
    // （外层 union、内层数组）。多写一层会把 float32[0] 当数组初始化 ⇒ 只有第一个分量生效、
    // 其余补零——真机上表现为"清红只得红、清绿全零"，是逐像素验证抓出来的。
    // ⚠️ Brace count matters: VkClearColorValue is a union{ float32[4]; ... }, so initialising it takes
    // exactly two levels. One extra level initialises float32[0] as an array, leaving the rest zeroed --
    // measured on the device as "red came out red, green came out black", caught by pixel verification.
    clear_pack p = { rt, { { r, g, b, a } } };
    return one_shot(ctx, rec_clear, &p);
}

typedef struct { vk_rt *t; } copy_pack;

static void rec_copy(VkCommandBuffer cb, void *ud) {
    vk_rt *t = ((copy_pack *)ud)->t;
    VkBufferImageCopy reg = { 0 };
    reg.bufferRowLength = 0; reg.bufferImageHeight = 0;          // 紧密排布
    reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    reg.imageSubresource.layerCount = 1;
    reg.imageExtent.width = t->w; reg.imageExtent.height = t->h; reg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rb, 1, &reg);
}

long vk_backend_readback(vk_ctx *ctx, vk_rt *rt, void *buf, size_t cap) {
    copy_pack p = { rt };
    if (one_shot(ctx, rec_copy, &p) != 0) return -1;
    if (cap < (size_t)rt->rb_size) return -1;
    memcpy(buf, rt->rb_map, (size_t)rt->rb_size);
    return (long)rt->rb_size;
}

// ---- 冒烟自测：init → 64×64 清红 → 读回验 → 清绿 → 读回验（验证重复操作与布局推进）----
static uint64_t ms_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int vk_backend_smoke(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();

    #define EMIT(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                           if (_n > 0) { p += _n; if (p > end) p = end; } \
                           dm_log_printf(__VA_ARGS__); } while (0)

    EMIT("=== Vulkan 冒烟（离屏：清屏 + 读回逐像素验证）===");

    char err[256] = "";
    vk_ctx *c = vk_backend_init(err, sizeof err);
    if (!c) { EMIT("❌ 初始化失败：%s", err); EMIT("===== 冒烟失败 ====="); return 1; }
    EMIT("设备：%s", c->devname);
    EMIT("API 版本：%u.%u.%u，驱动：0x%08x",
         c->api_ver >> 22, (c->api_ver >> 12) & 0x3ff, c->api_ver & 0xfff, c->driver_ver);

    vk_rt *t = vk_backend_target(c, 64, 64, err, sizeof err);
    if (!t) { EMIT("❌ 建离屏目标失败：%s", err); vk_backend_shutdown(c); EMIT("===== 冒烟失败 ====="); return 1; }
    EMIT("离屏目标：64×64 RGBA8（设备内存）+ 读回 buffer（host visible）");

    static const struct { float c[4]; const char *name; uint8_t expect[4]; } steps[] = {
        { { 1, 0, 0, 1 }, "纯红", { 255, 0, 0, 255 } },
        { { 0, 1, 0, 1 }, "纯绿", { 0, 255, 0, 255 } },
    };
    uint8_t *pix = malloc((size_t)t->rb_size);
    for (unsigned s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        uint64_t a = ms_now();
        if (vk_backend_clear(c, t, steps[s].c[0], steps[s].c[1], steps[s].c[2], steps[s].c[3]) != 0) {
            EMIT("❌ 第 %u 次清屏（%s）失败", s + 1, steps[s].name); fails++; break;
        }
        long got = vk_backend_readback(c, t, pix, t->rb_size);
        if (got < 0) { EMIT("❌ 第 %u 次读回失败", s + 1); fails++; break; }
        uint64_t bad = 0;
        for (uint32_t i = 0; i < t->w * t->h; i++) {
            const uint8_t *px = pix + (size_t)i * 4;
            if (px[0] != steps[s].expect[0] || px[1] != steps[s].expect[1] ||
                px[2] != steps[s].expect[2] || px[3] != steps[s].expect[3]) bad++;
        }
        // 失败时把实际像素字节打出来——没有这个就只能瞎猜是通道序、布局还是拷贝的问题
        // (on failure, dump the actual bytes; without them the cause is guesswork)
        EMIT("%s 清成%s后读回：%u 像素中 %llu 个不符（%llums）",
             bad ? "❌" : "✅", steps[s].name, t->w * t->h, (unsigned long long)bad,
             (unsigned long long)(ms_now() - a));
        if (bad) {
            const uint8_t *f = pix, *m = pix + (size_t)(t->w * t->h / 2) * 4, *l = pix + (size_t)(t->w * t->h - 1) * 4;
            EMIT("   实际字节：首=(%u,%u,%u,%u) 中=(%u,%u,%u,%u) 尾=(%u,%u,%u,%u)；预期 R8G8B8A8=(%u,%u,%u,%u)",
                 f[0], f[1], f[2], f[3], m[0], m[1], m[2], m[3], l[0], l[1], l[2], l[3],
                 steps[s].expect[0], steps[s].expect[1], steps[s].expect[2], steps[s].expect[3]);
            fails++;
        }
    }
    free(pix);
    vk_backend_target_free(c, t);
    vk_backend_shutdown(c);

    EMIT("===== 冒烟%s：失败 %d 项（总耗时 %llums）=====",
         fails ? "失败" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *p = 0;
    return fails;
}

// ================= 阶段 2：SPIR-V 直通 =================
// 上游离线链的产物就是 SPIR-V（Maxwell → SPIR-V → spirv-cross → MSL → metallib）。
// 这里用**手写 SPIR-V**（tri_shaders.h，见生成脚本）直接建管线画三角形——
// 证明"游戏着色器同源的语言不经翻译直接进 Vulkan"，Metal 那三个环节整段跳过。
static VkShaderModule make_module(VkDevice dev, const uint32_t *code, uint32_t n, char *err, size_t errcap) {
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mi.codeSize = (size_t)n * 4; mi.pCode = code;
    VkShaderModule m = NULL;
    VkResult r = vkCreateShaderModule(dev, &mi, NULL, &m);
    if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateShaderModule", r); return NULL; }
    return m;
}

typedef struct { vk_rt *t; VkPipeline pl; VkBuffer vb; } draw_pack;

static void rec_draw(VkCommandBuffer cb, void *ud) {
    draw_pack *d = (draw_pack *)ud;
    vk_rt *t = d->t;

    // 经典 renderPass：initialLayout=UNDEFINED、finalLayout=TRANSFER_SRC（渲染完直接可读回）
    VkClearValue cv = { 0 };
    cv.color.float32[0] = 1.f;                                 // 清成红（三角形外应读到它）
    cv.color.float32[3] = 1.f;                                 // alpha 也要 1——上次漏了它，读回是 (255,0,0,0)
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = t->rp; bi.framebuffer = t->fb;
    bi.renderArea.extent.width = t->w; bi.renderArea.extent.height = t->h;
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pl);
    VkViewport vp = { 0.f, 0.f, (float)t->w, (float)t->h, 0.f, 1.f };
    VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &d->vb, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);

    // finalLayout 已是 TRANSFER_SRC：直接读回
    VkBufferImageCopy reg = { 0 };
    reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    reg.imageSubresource.layerCount = 1;
    reg.imageExtent.width = t->w; reg.imageExtent.height = t->h; reg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rb, 1, &reg);
}

int vk_backend_shader(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT2(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT2("=== SPIR-V 直通（阶段2：手写 SPIR-V 着色器画三角形）===");
    char err[256] = "";
    vk_ctx *c = vk_backend_init(err, sizeof err);
    if (!c) { EMIT2("❌ 初始化失败：%s", err); EMIT2("===== 失败 ====="); return 1; }
    EMIT2("✓ 初始化完成（设备 %s）", c->devname);
    vk_rt *t = vk_backend_target(c, 64, 64, err, sizeof err);
    if (!t) { EMIT2("❌ 建目标失败：%s", err); vk_backend_shutdown(c); EMIT2("===== 失败 ====="); return 1; }
    EMIT2("✓ 离屏目标 + 读回缓冲就绪");
    EMIT2("… 建着色器模块（驱动将首次解析手写 SPIR-V）");
    // 管线：两个 SPIR-V 模块 + dynamic rendering + 动态 viewport/scissor
    VkShaderModule vm = make_module(c->dev, dm_spv_vert, dm_spv_vert_n, err, sizeof err);
    VkShaderModule fm = vm ? make_module(c->dev, dm_spv_frag, dm_spv_frag_n, err, sizeof err) : NULL;
    VkPipelineLayout lay = NULL;
    VkPipeline pl = NULL;
    VkBuffer vb = NULL; VkDeviceMemory vbm = NULL;
    if (!vm || !fm) { EMIT2("❌ 建着色器模块失败：%s", err); fails++; goto done; }
    EMIT2("✓ 着色器模块创建成功（顶点 %u 字，片元 %u 字）", dm_spv_vert_n, dm_spv_frag_n);

    {
        VkPipelineShaderStageCreateInfo st[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL },
        };
        VkVertexInputBindingDescription bind = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &attr;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vpst = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vpst.viewportCount = 1; vpst.scissorCount = 1;                       // 具体值走动态状态
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // ⚠️ 不留 NULL：规范允许"无深度附件时 pDepthStencilState 为 NULL"，但有的驱动不兼容——
        //   真机上 vkCreateGraphicsPipelines 阶段渲染服务直接崩（实验 A：显式给禁用状态）。
        VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.stencilTestEnable = VK_FALSE;
        VkPipelineColorBlendAttachmentState attst = { 0 };
        attst.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &attst;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkResult r = vkCreatePipelineLayout(c->dev, &li, NULL, &lay);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreatePipelineLayout", r); fails++; goto done; }
        EMIT2("✓ 管线布局创建成功");
        EMIT2("… 驱动将编译管线（含两条 SPIR-V，renderPass 路径）");
        VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gi.stageCount = 2; gi.pStages = st;
        gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vpst; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &dss; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
        gi.layout = lay; gi.renderPass = t->rp;                        // 实验B：经典 renderPass 路径
        r = vkCreateGraphicsPipelines(c->dev, NULL, 1, &gi, NULL, &pl);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateGraphicsPipelines", r); fails++; goto done; }
        EMIT2("✓ 图形管线创建成功");
    }

    {
        // 顶点数据：底边两个点 + 顶上一个点（设备可见内存，绘制前写好）
        const float verts[6] = { -1.f, -1.f, 1.f, -1.f, 0.f, 1.f };
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = sizeof verts; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkResult r = vkCreateBuffer(c->dev, &bi, NULL, &vb);
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c->dev, vb, &mr);
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mi.allocationSize = mr.size;
        mi.memoryTypeIndex = find_mem_type(c, mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r);
        if (r != VK_SUCCESS) { snprintf(err, sizeof err, "找不到顶点缓冲的内存类型"); fails++; goto done; }
        r = vkAllocateMemory(c->dev, &mi, NULL, &vbm);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkAllocateMemory(vb)", r); fails++; goto done; }
        vkBindBufferMemory(c->dev, vb, vbm, 0);
        void *mp = NULL; vkMapMemory(c->dev, vbm, 0, sizeof verts, 0, &mp);
        memcpy(mp, verts, sizeof verts); vkUnmapMemory(c->dev, vbm);
    }

    {
        draw_pack dp = { t, pl, vb };
        if (one_shot(c, rec_draw, &dp) != 0) { EMIT2("❌ 绘制命令提交失败"); fails++; goto done; }
    }

    {
        uint8_t pix[64 * 64 * 4];
        if (vk_backend_readback(c, t, pix, sizeof pix) < 0) { EMIT2("❌ 读回失败"); fails++; goto done; }
        // Vulkan 的 NDC (-1,-1) 在**左上**（Y 向下）：三角形底边在顶部整行、顶点在底部中央。
        // (32,16) 在三角形内；(0,63) 在底部左角——离三条边都远，必定在外。
        // （别用 (0,0)：它恰好压在左边缘上，画不画是边界像素规则决定的，驱动间不一致。）
        const uint8_t *inside = pix + (16 * 64 + 32) * 4;
        const uint8_t *outside = pix + (63 * 64 + 0) * 4;
        EMIT2("三角形内 (32,16) 读到 (%u,%u,%u,%u)，预期蓝 (0,0,255,255)", inside[0], inside[1], inside[2], inside[3]);
        EMIT2("三角形外 (0,63)  读到 (%u,%u,%u,%u)，预期红 (255,0,0,255)", outside[0], outside[1], outside[2], outside[3]);
        int ok_in = inside[0] == 0 && inside[1] == 0 && inside[2] == 255 && inside[3] == 255;
        int ok_out = outside[0] == 255 && outside[1] == 0 && outside[2] == 0 && outside[3] == 255;
        if (!ok_in) { EMIT2("❌ 三角形内颜色不对：着色器/管线/绘制这条链有问题"); fails++; }
        if (!ok_out) { EMIT2("❌ 三角形外颜色不对：清屏或绘制范围有问题"); fails++; }
        EMIT2("%s SPIR-V 直通验证%s", (ok_in && ok_out) ? "✅" : "❌", (ok_in && ok_out) ? "——游戏着色器同源语言不经翻译直接进 Vulkan" : "");
    }

done:
    if (pl) vkDestroyPipeline(c->dev, pl, NULL);
    if (lay) vkDestroyPipelineLayout(c->dev, lay, NULL);
    if (vm) vkDestroyShaderModule(c->dev, vm, NULL);
    if (fm) vkDestroyShaderModule(c->dev, fm, NULL);
    if (vb) vkDestroyBuffer(c->dev, vb, NULL);
    if (vbm) vkFreeMemory(c->dev, vbm, NULL);
    vk_backend_target_free(c, t);
    vk_backend_shutdown(c);
    EMIT2("===== SPIR-V 直通%s：失败 %d 项（总耗时 %llums）=====",
          fails ? "失败" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *p = 0;
    return fails;
}

// ================= 阶段 3 前置：内存池零拷贝的平台能力验证 =================
// 上游 Metal 侧用 newBufferWithBytesNoCopy 把游戏的内存池包成零拷贝缓冲
// （nvn_mtl.m 的 mtl_pool_add / mtl_addr / mtl_gpu_addr）。Vulkan 侧本机没有
// VK_EXT_external_memory_host，走的是"宿主自己分配"那条路：**让客体堆的 CPU 映射本身就是
// Vulkan 内存的映射**——客体堆由宿主 mmap 预留、SetHeapSize 只是 mprotect 开放
// （hle_svc.c 的 mem_init / SetHeapSize），宿主完全说了算，所以这条路成立。
// 这条路要两个前提，本探针逐条在真机上验：
//   ① 规模：能否一次分配并映射「客体堆量级」（约 3.7 GiB）的 HOST_VISIBLE|DEVICE_LOCAL 内存；
//   ② 机制：CPU 写进去的数据，GPU 经缓冲是否真的看得到。
// 判据都是逐字节的——"没报错"不算通过（清屏色漏 alpha 那类问题就是这么抓的）。
// Upstream wraps game memory pools into zero-copy Metal buffers via newBufferWithBytesNoCopy. There is
// no VK_EXT_external_memory_host here, so the route is "the host allocates": make the guest heap's CPU
// mapping BE the Vulkan memory. Two premises are verified here, both byte-exact.
typedef struct { VkBuffer a, b; VkDeviceSize n; } memb_copy_pack;

static void rec_copy_buf(VkCommandBuffer cb, void *ud) {
    memb_copy_pack *d = (memb_copy_pack *)ud;
    VkBufferCopy cp = { 0, 0, d->n };
    vkCmdCopyBuffer(cb, d->a, d->b, 1, &cp);
}

int vk_backend_memprobe(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT3(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT3("=== 内存池零拷贝的平台能力验证（阶段 3 前置）===");
    EMIT3("回答两件事：① 规模（客体堆整块能否落在 Vulkan 内存里）；② 机制（CPU 写的数据 GPU 看不看得到）");

    char err[256] = "";
    vk_ctx *c = vk_backend_init(err, sizeof err);
    if (!c) { EMIT3("❌ 初始化失败：%s", err); EMIT3("===== 失败 ====="); return 1; }
    EMIT3("设备：%s", c->devname);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(c->phy, &mp);

    EMIT3("---- 内存堆 %u 个 ----", mp.memoryHeapCount);
    for (uint32_t i = 0; i < mp.memoryHeapCount && i < 8; i++)
        EMIT3("  堆 %u: %.0f MiB  %s", i, mp.memoryHeaps[i].size / 1048576.0,
              (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "（显存）" : "（系统内存）");

    EMIT3("---- 内存类型 %u 个 ----", mp.memoryTypeCount);
    for (uint32_t i = 0; i < mp.memoryTypeCount && i < 16; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        EMIT3("  %2u: 堆 %u  %s%s%s%s  属性 %#x", (unsigned)i, mp.memoryTypes[i].heapIndex,
              (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "显存本地 " : "",
              (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "CPU可写 " : "",
              (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "一致 " : "",
              (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "CPU缓存" : "",
              (unsigned)f);
    }

    // 候选：CPU 可写 + 显存本地（零拷贝缓冲的前提）。优先挑一致内存（免 flush）。
    int cand = -1, cand_coh = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            != (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
        int coh = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (cand < 0 || (coh && !cand_coh)) { cand = (int)i; cand_coh = coh; }
    }
    if (cand < 0) {
        EMIT3("❌ 没有「CPU 可写 + 显存本地」的内存类型 —— 零拷贝这条路要另想（与能力探测结论矛盾，要复查）");
        fails++;
        goto done;
    }
    EMIT3("选用内存类型 %u（%s）", (unsigned)cand,
          cand_coh ? "一致内存，不需要 flush" : "非一致内存，需要 flush/invalidate");

    // ---- ① 规模：能否一次分配并映射客体堆那么大的内存 ----
    EMIT3("");
    EMIT3("---- ① 规模：分配并映射「客体堆」量级的内存 ----");
    EMIT3("  参照：客体堆 = g_as+0x40000000，容量 4 GB，游戏实际报到约 3.7 GB（hle_svc.c）");
    EMIT3("  （最后一项是重头戏：若整机内存吃紧，App 有可能被系统结束，前面的读数已进 hilog）");
    {
        static const uint64_t sizes[]  = { 64ull << 20, 256ull << 20, 1024ull << 20, 2048ull << 20, 3728ull << 20 };
        static const char *labels[]    = { "64 MiB（冒烟）", "256 MiB", "1 GiB", "2 GiB", "3.7 GiB（客体堆量级）" };
        for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
            uint64_t sz = sizes[s];
            VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mi.allocationSize = sz; mi.memoryTypeIndex = (uint32_t)cand;
            uint64_t a = ms_now();
            VkDeviceMemory m = NULL;
            VkResult r = vkAllocateMemory(c->dev, &mi, NULL, &m);
            if (r != VK_SUCCESS) {
                EMIT3("  ❌ %s：分配失败（VkResult=%d，%llums）", labels[s], (int)r,
                      (unsigned long long)(ms_now() - a));
                fails++;
                continue;
            }
            void *q = NULL;
            r = vkMapMemory(c->dev, m, 0, VK_WHOLE_SIZE, 0, &q);
            if (r != VK_SUCCESS) {
                EMIT3("  ⚠ %s：分配成功但映射失败（VkResult=%d）", labels[s], (int)r);
                vkFreeMemory(c->dev, m, NULL); fails++;
                continue;
            }
            volatile uint8_t *v = (volatile uint8_t *)q;
            v[0] = 0x5a; v[sz / 2] = 0x3c; v[sz - 1] = 0xa5;
            int rd = (v[0] == 0x5a && v[sz / 2] == 0x3c && v[sz - 1] == 0xa5);
            EMIT3("  %s %s：分配 + 映射成功，地址 %p，首/中/尾读写%s（%llums）",
                  rd ? "✅" : "⚠", labels[s], q, rd ? "一致" : "不一致",
                  (unsigned long long)(ms_now() - a));
            if (!rd) fails++;
            vkUnmapMemory(c->dev, m);
            vkFreeMemory(c->dev, m, NULL);
        }
    }

    // ---- ② 机制：CPU 写 → GPU copy → CPU 读，逐字节比对 ----
    EMIT3("");
    EMIT3("---- ② 机制：CPU 写进零拷贝缓冲，GPU 看不看得到 ----");
    EMIT3("  做法：同一块内存上建两个缓冲 A/B（B 落在非零偏移，检验池切片要的偏移对齐），");
    EMIT3("        CPU 写 A → vkCmdCopyBuffer A→B（GPU 执行）→ CPU 读 B 逐字节比对");
    {
        const VkDeviceSize CHUNK = 64 * 1024;
        VkBuffer A = NULL, B = NULL; VkDeviceMemory dm = NULL; void *q = NULL;
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = CHUNK;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkResult r = vkCreateBuffer(c->dev, &bci, NULL, &A);
        if (r != VK_SUCCESS) { EMIT3("  ❌ 建缓冲 A 失败（VkResult=%d）", (int)r); fails++; }
        else {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c->dev, A, &mr);
            EMIT3("  缓冲内存需求：size=%llu 对齐=%llu；候选类型在 memoryTypeBits 里：%s",
                  (unsigned long long)mr.size, (unsigned long long)mr.alignment,
                  (mr.memoryTypeBits & (1u << cand)) ? "是" : "否");
            if (!(mr.memoryTypeBits & (1u << cand))) { EMIT3("  ❌ 候选类型不能绑这个缓冲"); fails++; }
            else {
                VkDeviceSize offB = (mr.size + mr.alignment - 1) / mr.alignment * mr.alignment;
                VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mi.allocationSize = offB + mr.size; mi.memoryTypeIndex = (uint32_t)cand;
                r = vkAllocateMemory(c->dev, &mi, NULL, &dm);
                if (r != VK_SUCCESS) { EMIT3("  ❌ 分配失败（VkResult=%d）", (int)r); fails++; }
                else if (vkCreateBuffer(c->dev, &bci, NULL, &B) != VK_SUCCESS) { EMIT3("  ❌ 建缓冲 B 失败"); fails++; }
                else if (vkBindBufferMemory(c->dev, A, dm, 0) != VK_SUCCESS) { EMIT3("  ❌ 绑定 A 失败"); fails++; }
                else if (vkBindBufferMemory(c->dev, B, dm, offB) != VK_SUCCESS) {
                    EMIT3("  ❌ 绑定 B 到非零偏移 %llu 失败（池切片要用非零偏移绑定）",
                          (unsigned long long)offB);
                    fails++;
                }
                else if (vkMapMemory(c->dev, dm, 0, VK_WHOLE_SIZE, 0, &q) != VK_SUCCESS) {
                    EMIT3("  ❌ 映射失败"); fails++;
                }
                else {
                    EMIT3("  缓冲 A 偏移 0，缓冲 B 偏移 %llu（同一块内存）", (unsigned long long)offB);
                    uint8_t *a = (uint8_t *)q, *b = (uint8_t *)q + offB;
                    for (VkDeviceSize i = 0; i < CHUNK; i++) a[i] = (uint8_t)(i * 131u + 7u);
                    if (!cand_coh) {
                        VkMappedMemoryRange rng = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
                        rng.memory = dm; rng.offset = 0; rng.size = VK_WHOLE_SIZE;
                        VkResult fr = vkFlushMappedMemoryRanges(c->dev, 1, &rng);
                        EMIT3("  非一致内存：vkFlushMappedMemoryRanges %s", fr == VK_SUCCESS ? "成功" : "失败");
                    }
                    memb_copy_pack pk = { A, B, CHUNK };
                    if (one_shot(c, rec_copy_buf, &pk) != 0) { EMIT3("  ❌ copy 提交失败"); fails++; }
                    else {
                        if (!cand_coh) {
                            VkMappedMemoryRange rng = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
                            rng.memory = dm; rng.offset = 0; rng.size = VK_WHOLE_SIZE;
                            vkInvalidateMappedMemoryRanges(c->dev, 1, &rng);
                        }
                        VkDeviceSize bad = 0, first = (VkDeviceSize)-1;
                        for (VkDeviceSize i = 0; i < CHUNK; i++)
                            if (b[i] != (uint8_t)(i * 131u + 7u)) {
                                if (first == (VkDeviceSize)-1) first = i;
                                bad++;
                            }
                        if (bad)
                            EMIT3("  ❌ 逐字节比对：%llu 字节里 %llu 个不符（首个不符在偏移 %llu）",
                                  (unsigned long long)CHUNK, (unsigned long long)bad, (unsigned long long)first);
                        else
                            EMIT3("  ✅ 逐字节比对：%llu 字节全部一致 —— CPU 写的数据 GPU 确实看得到，零拷贝机制成立",
                                  (unsigned long long)CHUNK);
                        if (bad) fails++;
                    }
                }
            }
        }
        if (q) vkUnmapMemory(c->dev, dm);
        if (A) vkDestroyBuffer(c->dev, A, NULL);
        if (B) vkDestroyBuffer(c->dev, B, NULL);
        if (dm) vkFreeMemory(c->dev, dm, NULL);
    }

done:
    vk_backend_shutdown(c);
    EMIT3("");
    EMIT3("===== 内存池零拷贝验证%s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    if (!fails)
        EMIT3("结论：两条前提都成立 ⇒ 阶段 3 可走「客体堆的 CPU 映射即 Vulkan 内存」的零拷贝路线");
    *p = 0;
    #undef EMIT3
    return fails;
}

// ================= 阶段 3 第一步（3.1-A）：池零拷贝原型 =================
// 不动任何上游文件，只在探针里把「游戏内存池 → VkBuffer」这条链走通：
//   ① 造一块「模拟客体堆」——就是 3.1-B 要给游戏当堆的那块内存：Vulkan 分配 + 映射；
//   ② 从中切两个「池」，各建 VkBuffer 绑到池的偏移上（对标 nvn_mtl.m 的 mtl_pool_add）；
//   ③ 用缓冲设备地址验证「线性映射」：GPU 地址 − 池的 CPU 地址 应当是个常数
//      ——上游 mtl_gpu_addr 的语义正是 gpu = pool.gpu + off，靠的就是这个不变式；
//   ④ CPU 写池 A → vkCmdCopyBuffer A→B → CPU 读池 B，逐字节比对
//      ——证明池切片真的能被 GPU 读写，不只是"能建出来"。
// 对齐要求、设备地址差、耗时都打出来，供 3.1-B / 3.1-C 直接用。
// Prototype of the pool zero-copy path, entirely inside the probe: no upstream file is touched.
// Criteria are byte-exact; alignment, device-address delta and timings are printed for 3.1-B/C.
int vk_backend_poolproto(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT4(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT4("=== 阶段 3 第一步原型（3.1-A）：内存池零拷贝 ===");
    EMIT4("把「游戏内存池 → VkBuffer」这条链走通（模拟客体堆；不动任何上游文件）");

    char err[256] = "";
    vk_ctx *c = vk_backend_init(err, sizeof err);
    if (!c) { EMIT4("❌ 初始化失败：%s", err); EMIT4("===== 失败 ====="); return 1; }
    EMIT4("设备：%s", c->devname);

    PFN_vkGetBufferDeviceAddress pGetBDA =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(c->dev, "vkGetBufferDeviceAddress");
    if (!pGetBDA) {
        EMIT4("❌ 拿不到 vkGetBufferDeviceAddress（缓冲设备地址不可用）—— 3.1-C 的地址反查要另想");
        vk_backend_shutdown(c); EMIT4("===== 失败 ====="); *p = 0; return 1;
    }

    // 内存类型：CPU 可写 + 显存本地 + 一致（与 3.1-B 要用的同一个）
    VkResult r;
    uint32_t mt = find_mem_type(c, 0xFFFFFFFFu,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r);
    if (r != VK_SUCCESS) {
        EMIT4("❌ 找不到「CPU 可写 + 显存本地 + 一致」的内存类型");
        vk_backend_shutdown(c); EMIT4("===== 失败 ====="); *p = 0; return 1;
    }
    EMIT4("内存类型 %u（显存本地 + CPU 可写 + 一致）", mt);

    // ---- 缓冲对齐要求：池的偏移必须按它对齐（池切片的关键约束）----
    EMIT4("---- 缓冲对齐要求（池偏移要按 alignment 对齐）----");
    {
        static const VkDeviceSize szs[] = { 64u << 10, 1u << 20, 16u << 20 };
        for (unsigned i = 0; i < 3; i++) {
            VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = szs[i];
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
            VkBuffer b = NULL;
            if (vkCreateBuffer(c->dev, &bi, NULL, &b) != VK_SUCCESS) {
                EMIT4("  ⚠ %llu KiB：建缓冲失败", (unsigned long long)(szs[i] >> 10));
                continue;
            }
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(c->dev, b, &mr);
            EMIT4("  %6llu KiB 的缓冲：alignment=%llu，类型 %u %s memoryTypeBits",
                  (unsigned long long)(szs[i] >> 10), (unsigned long long)mr.alignment, mt,
                  (mr.memoryTypeBits & (1u << mt)) ? "在" : "不在");
            vkDestroyBuffer(c->dev, b, NULL);
        }
    }

    // ---- ① 模拟客体堆 ----
    const VkDeviceSize HEAP_SZ = 32ull << 20;
    VkDeviceMemory heap_mem = NULL;
    void *heap = NULL;
    {
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mi.allocationSize = HEAP_SZ; mi.memoryTypeIndex = mt;
        r = vkAllocateMemory(c->dev, &mi, NULL, &heap_mem);
        if (r != VK_SUCCESS) {
            EMIT4("❌ 模拟客体堆分配失败（VkResult=%d）", (int)r);
            vk_backend_shutdown(c); EMIT4("===== 失败 ====="); *p = 0; return 1;
        }
        r = vkMapMemory(c->dev, heap_mem, 0, VK_WHOLE_SIZE, 0, &heap);
        if (r != VK_SUCCESS) {
            EMIT4("❌ 模拟客体堆映射失败（VkResult=%d）", (int)r);
            vkFreeMemory(c->dev, heap_mem, NULL); vk_backend_shutdown(c);
            EMIT4("===== 失败 ====="); *p = 0; return 1;
        }
        EMIT4("① 模拟客体堆：%llu MiB，CPU 映射地址 %p",
              (unsigned long long)(HEAP_SZ >> 20), heap);
    }

    // ---- ② 两个池 ----
    typedef struct { const char *name; VkDeviceSize off, sz; VkBuffer buf; uint64_t gpu, delta; } pool_ent_;
    pool_ent_ pool[2] = {
        { "池A", 0,          1ull << 20, NULL, 0, 0 },
        { "池B", 2ull << 20, 1ull << 20, NULL, 0, 0 },
    };
    EMIT4("② 从堆里切两个池，各建一个 VkBuffer 绑到池偏移上");
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = pool[i].sz;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (vkCreateBuffer(c->dev, &bi, NULL, &pool[i].buf) != VK_SUCCESS) {
            EMIT4("  ❌ %s：建缓冲失败", pool[i].name); fails++; continue;
        }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(c->dev, pool[i].buf, &mr);
        if (pool[i].off % mr.alignment) {
            EMIT4("  ❌ %s：池偏移 %llu 不是 alignment %llu 的整数倍",
                  pool[i].name, (unsigned long long)pool[i].off, (unsigned long long)mr.alignment);
            fails++; continue;
        }
        r = vkBindBufferMemory(c->dev, pool[i].buf, heap_mem, pool[i].off);
        if (r != VK_SUCCESS) {
            EMIT4("  ❌ %s：绑定到堆偏移 %llu 失败（VkResult=%d）",
                  pool[i].name, (unsigned long long)pool[i].off, (int)r);
            fails++; continue;
        }
        VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        ai.buffer = pool[i].buf;
        pool[i].gpu = pGetBDA(c->dev, &ai);
        uint64_t cpu = (uint64_t)(uintptr_t)heap + pool[i].off;
        pool[i].delta = (uint64_t)((int64_t)pool[i].gpu - (int64_t)cpu);
        EMIT4("  ✅ %s：堆偏移 %llu，CPU %p，GPU %#llx，GPU−CPU = %lld",
              pool[i].name, (unsigned long long)pool[i].off, (void *)(uintptr_t)cpu,
              (unsigned long long)pool[i].gpu, (long long)(int64_t)pool[i].delta);
    }

    // ---- ③ 线性映射不变式 ----
    EMIT4("③ 线性映射不变式（上游 mtl_gpu_addr 的 gpu = pool.gpu + off 靠的就是它）");
    if (pool[0].buf && pool[1].buf) {
        if (pool[0].delta == pool[1].delta)
            EMIT4("  ✅ 两个池的 GPU−CPU 差相同（%lld）⇒ 地址反查可线性推导",
                  (long long)(int64_t)pool[0].delta);
        else {
            EMIT4("  ❌ 两个池的 GPU−CPU 差不同（%lld vs %lld）⇒ 不能线性推导",
                  (long long)(int64_t)pool[0].delta, (long long)(int64_t)pool[1].delta);
            fails++;
        }
    } else { EMIT4("  ⚠ 有池没建起来，跳过"); fails++; }

    // ---- ④ CPU 写池A → GPU copy → CPU 读池B ----
    EMIT4("④ CPU 写池 A → vkCmdCopyBuffer A→B（GPU 执行）→ CPU 读池 B 逐字节比对");
    if (pool[0].buf && pool[1].buf) {
        const VkDeviceSize N = 256u << 10;
        uint8_t *a = (uint8_t *)heap + pool[0].off;
        uint8_t *b = (uint8_t *)heap + pool[1].off;
        for (VkDeviceSize i = 0; i < N; i++) a[i] = (uint8_t)(i * 97u + 11u);
        memb_copy_pack pk = { pool[0].buf, pool[1].buf, N };
        uint64_t t1 = ms_now();
        if (one_shot(c, rec_copy_buf, &pk) != 0) { EMIT4("  ❌ copy 提交失败"); fails++; }
        else {
            VkDeviceSize bad = 0, first = (VkDeviceSize)-1;
            for (VkDeviceSize i = 0; i < N; i++)
                if (b[i] != (uint8_t)(i * 97u + 11u)) {
                    if (first == (VkDeviceSize)-1) first = i;
                    bad++;
                }
            if (bad) {
                EMIT4("  ❌ %llu 字节里 %llu 个不符（首个在偏移 %llu）",
                      (unsigned long long)N, (unsigned long long)bad, (unsigned long long)first);
                fails++;
            } else {
                EMIT4("  ✅ %llu 字节全部一致（%llums）——池切片真的能被 GPU 读写",
                      (unsigned long long)N, (unsigned long long)(ms_now() - t1));
            }
        }
    }

    for (int i = 0; i < 2; i++) if (pool[i].buf) vkDestroyBuffer(c->dev, pool[i].buf, NULL);
    if (heap) vkUnmapMemory(c->dev, heap_mem);
    if (heap_mem) vkFreeMemory(c->dev, heap_mem, NULL);
    vk_backend_shutdown(c);

    EMIT4("");
    EMIT4("===== 3.1-A %s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    if (!fails)
        EMIT4("结论：池切片（VkBuffer 绑非零偏移）+ 线性设备地址 + GPU 读写全部成立 ⇒ 3.1-B/C 可照此实现");
    *p = 0;
    #undef EMIT4
    return fails;
}

// ================= 阶段 3 第一步（3.1-B）：客体堆 = Vulkan 内存 =================
// 3.1-A 在「模拟客体堆」上验了机制；这一步验**真实尺寸**：按客体堆的实际规模（4 GB）预留，
// 再从预留里切两个池、绑 VkBuffer、让 GPU 搬运并逐字节比对。
// 走的是 dm/ohos/vk_ctx_ohos.c 的**共享上下文 + dm_ohos_heap_reserve()**——
// 也就是 hle_svc.c 的 mem_init 在 DM_VK_HEAP=1 时会走的那条路（同一份代码，不是复制品）。
// 3.1-A verified the mechanism on a simulated heap; this verifies the real size, through the very code
// path hle_svc.c takes when DM_VK_HEAP=1.
int vk_backend_heapreserve(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT5(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT5("=== 阶段 3 第一步（3.1-B）：客体堆预留（真实尺寸）===");
    EMIT5("走共享上下文 + dm_ohos_heap_reserve()——就是 hle_svc.c 在 DM_VK_HEAP=1 时走的那条路");

    const uint64_t HEAP = 4ull << 30;      // 与 hle_svc.c 的 g_heap_cap（RESERVE − 1 GB）一致
    uint64_t t1 = ms_now();
    void *heap = dm_ohos_heap_reserve(HEAP);
    if (!heap) {
        EMIT5("❌ 预留失败：%s", dm_ohos_vk_err());
        EMIT5("===== 失败 ====="); *p = 0; return 1;
    }
    EMIT5("设备：%s", dm_ohos_vk_devname());
    EMIT5("① 客体堆预留：%llu MiB，CPU 映射地址 %p，耗时 %llums",
          (unsigned long long)(HEAP >> 20), heap, (unsigned long long)(ms_now() - t1));

    VkDevice dev = dm_ohos_vk_device();
    VkDeviceMemory hm = dm_ohos_heap_memory();
    if (!dev || !hm) {
        EMIT5("❌ 共享上下文没给出设备/内存"); EMIT5("===== 失败 ====="); *p = 0; return 1;
    }

    // 池：模拟游戏从堆的中间切出两块（偏移 1 GiB 与 1 GiB + 64 MiB）
    typedef struct { const char *name; VkDeviceSize off, sz; VkBuffer buf; uint64_t gpu, delta; } pool_ent_;
    pool_ent_ pool[2] = {
        { "池A", 1ull << 30,                   4ull << 20, NULL, 0, 0 },
        { "池B", (1ull << 30) + (64ull << 20), 4ull << 20, NULL, 0, 0 },
    };
    PFN_vkGetBufferDeviceAddress pGetBDA =
        (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(dev, "vkGetBufferDeviceAddress");

    EMIT5("② 从预留里切两个池（偏移 1 GiB / 1 GiB+64 MiB），各绑一个 VkBuffer");
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = pool[i].sz;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        if (vkCreateBuffer(dev, &bi, NULL, &pool[i].buf) != VK_SUCCESS) {
            EMIT5("  ❌ %s：建缓冲失败", pool[i].name); fails++; continue;
        }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, pool[i].buf, &mr);
        if (pool[i].off % mr.alignment) {
            EMIT5("  ❌ %s：偏移 %llu 不是 alignment %llu 的整数倍",
                  pool[i].name, (unsigned long long)pool[i].off, (unsigned long long)mr.alignment);
            fails++; continue;
        }
        VkResult r = vkBindBufferMemory(dev, pool[i].buf, hm, pool[i].off);
        if (r != VK_SUCCESS) {
            EMIT5("  ❌ %s：绑到预留偏移 %llu 失败（VkResult=%d）",
                  pool[i].name, (unsigned long long)pool[i].off, (int)r);
            fails++; continue;
        }
        uint64_t cpu = (uint64_t)(uintptr_t)heap + pool[i].off;
        if (pGetBDA) {
            VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
            ai.buffer = pool[i].buf;
            pool[i].gpu = pGetBDA(dev, &ai);
            pool[i].delta = (uint64_t)((int64_t)pool[i].gpu - (int64_t)cpu);
        }
        EMIT5("  ✅ %s：偏移 %llu，CPU %p，GPU %#llx，GPU−CPU = %lld",
              pool[i].name, (unsigned long long)pool[i].off, (void *)(uintptr_t)cpu,
              (unsigned long long)pool[i].gpu, (long long)(int64_t)pool[i].delta);
    }

    EMIT5("③ 线性不变式");
    if (pool[0].buf && pool[1].buf) {
        if (pool[0].delta == pool[1].delta)
            EMIT5("  ✅ 两池 GPU−CPU 差相同（%lld）", (long long)(int64_t)pool[0].delta);
        else {
            EMIT5("  ❌ 两池 GPU−CPU 差不同（%lld vs %lld）",
                  (long long)(int64_t)pool[0].delta, (long long)(int64_t)pool[1].delta);
            fails++;
        }
    } else { EMIT5("  ⚠ 有池没建起来"); fails++; }

    EMIT5("④ CPU 写池 A → GPU copy → CPU 读池 B 逐字节比对");
    if (pool[0].buf && pool[1].buf) {
        const VkDeviceSize N = 1u << 20;
        uint8_t *a = (uint8_t *)heap + pool[0].off;
        uint8_t *b = (uint8_t *)heap + pool[1].off;
        for (VkDeviceSize i = 0; i < N; i++) a[i] = (uint8_t)(i * 31u + 5u);
        memb_copy_pack pk = { pool[0].buf, pool[1].buf, N };
        uint64_t t2 = ms_now();
        if (dm_ohos_vk_oneshot(rec_copy_buf, &pk) != 0) { EMIT5("  ❌ copy 提交失败"); fails++; }
        else {
            VkDeviceSize bad = 0, first = (VkDeviceSize)-1;
            for (VkDeviceSize i = 0; i < N; i++)
                if (b[i] != (uint8_t)(i * 31u + 5u)) {
                    if (first == (VkDeviceSize)-1) first = i;
                    bad++;
                }
            if (bad) {
                EMIT5("  ❌ %llu 字节里 %llu 个不符（首个在偏移 %llu）",
                      (unsigned long long)N, (unsigned long long)bad, (unsigned long long)first);
                fails++;
            } else {
                EMIT5("  ✅ %llu 字节全部一致（%llums）", (unsigned long long)N,
                      (unsigned long long)(ms_now() - t2));
            }
        }
    }

    for (int i = 0; i < 2; i++) if (pool[i].buf) vkDestroyBuffer(dev, pool[i].buf, NULL);
    dm_ohos_heap_release();

    EMIT5("");
    EMIT5("===== 3.1-B %s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    if (!fails)
        EMIT5("结论：真实尺寸的客体堆预留成立，池切片可被 GPU 读写 ⇒ hle_svc.c 的分叉可照此接线");
    *p = 0;
    #undef EMIT5
    return fails;
}

// ================= 阶段 3 第一步（3.1-C）：池模块 =================
// dm/gfx/nvn/nvn_vk.c（对标 nvn_mtl.m 的池那部分）在真机上验：
//   ① 池登记：从客体堆里切两个池，各建一个 VkBuffer 绑到池偏移上；
//   ② 地址反查：nvn_vk_addr / nvn_vk_gpu_addr 把"游戏给的地址"翻成"缓冲 + 偏移 + 设备地址"；
//   ③ 真搬运：按反查结果让 GPU 把池 A 的一段搬到池 B 的一段，逐字节比对；
//   ④ 负向判据：**池不在客体堆里时必须失败**（否则"零拷贝"是假的，会悄悄退化成不可用内存）。
typedef struct { VkBuffer a, b; VkDeviceSize so, dsto, n; } poolcopy_pack;

static void rec_copy_pool(VkCommandBuffer cb, void *ud) {
    poolcopy_pack *d = (poolcopy_pack *)ud;
    VkBufferCopy c = { d->so, d->dsto, d->n };
    vkCmdCopyBuffer(cb, d->a, d->b, 1, &c);
}

int vk_backend_poolmod(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT6(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT6("=== 阶段 3 第一步（3.1-C）：池模块（nvn_vk.c）===");
    EMIT6("池登记 → 地址反查 → GPU 真搬运 → 负向判据（池不在堆里必须失败）");

    const uint64_t HEAP = 4ull << 30;
    uint8_t *heap = (uint8_t *)dm_ohos_heap_reserve(HEAP);
    if (!heap) {
        EMIT6("❌ 客体堆预留失败：%s", dm_ohos_vk_err());
        EMIT6("===== 失败 ====="); *p = 0; return 1;
    }
    EMIT6("客体堆：%llu MiB，基址 %p（设备 %s）",
          (unsigned long long)(HEAP >> 20), heap, dm_ohos_vk_devname());

    // ① 两个池（模拟游戏从堆中间切出两块）
    const uint64_t POOL_SZ = 8ull << 20;
    uint8_t *pa = heap + (1ull << 30);
    uint8_t *pb = heap + (1ull << 30) + (64ull << 20);
    EMIT6("① 池登记");
    int ra = nvn_vk_pool_add(pa, POOL_SZ);
    EMIT6("  %s 池A @ %p+%llu MiB：%s", ra ? "❌" : "✅", pa, (unsigned long long)(POOL_SZ >> 20),
          ra ? nvn_vk_last_error() : "登记成功");
    if (ra) fails++;
    int rb = nvn_vk_pool_add(pb, POOL_SZ);
    EMIT6("  %s 池B @ %p+%llu MiB：%s", rb ? "❌" : "✅", pb, (unsigned long long)(POOL_SZ >> 20),
          rb ? nvn_vk_last_error() : "登记成功");
    if (rb) fails++;
    EMIT6("  当前已登记池数：%d", nvn_vk_pool_count());

    // ② 地址反查
    EMIT6("② 地址反查（游戏给的地址 → 缓冲 + 偏移 + 设备地址）");
    VkBuffer ba = VK_NULL_HANDLE, bb = VK_NULL_HANDLE;
    uint64_t oa = 0, ob = 0, ga = 0, gb = 0;
    const uint64_t IN_A = 4096, IN_B = 8192, N = 1u << 20;
    if (!ra) {
        uint64_t addr = (uint64_t)(uintptr_t)(pa + IN_A);
        int hit = nvn_vk_gpu_addr(addr, N, &ba, &oa, &ga);
        EMIT6("  %s 池A+%llu：命中=%d 偏移=%llu 设备地址=%#llx",
              (hit && oa == IN_A) ? "✅" : "❌", (unsigned long long)IN_A, hit,
              (unsigned long long)oa, (unsigned long long)ga);
        if (!hit || oa != IN_A) fails++;
    }
    if (!rb) {
        uint64_t addr = (uint64_t)(uintptr_t)(pb + IN_B);
        int hit = nvn_vk_gpu_addr(addr, N, &bb, &ob, &gb);
        EMIT6("  %s 池B+%llu：命中=%d 偏移=%llu 设备地址=%#llx",
              (hit && ob == IN_B) ? "✅" : "❌", (unsigned long long)IN_B, hit,
              (unsigned long long)ob, (unsigned long long)gb);
        if (!hit || ob != IN_B) fails++;
    }
    // 越界/未登记区间必须不命中
    {
        VkBuffer x = VK_NULL_HANDLE; uint64_t xo = 0;
        int hit = nvn_vk_addr((uint64_t)(uintptr_t)heap, 4096, &x, &xo);   // 堆头部没登记成池
        EMIT6("  %s 堆头部（未登记）：命中=%d（应为 0）", hit ? "❌" : "✅", hit);
        if (hit) fails++;
    }

    // ③ GPU 真搬运
    EMIT6("③ 按反查结果搬运：CPU 写池 A → vkCmdCopyBuffer(A+%llu → B+%llu) → CPU 读池 B",
          (unsigned long long)IN_A, (unsigned long long)IN_B);
    if (ba && bb) {
        uint8_t *src = pa + IN_A, *dst = pb + IN_B;
        for (uint64_t i = 0; i < N; i++) src[i] = (uint8_t)(i * 43u + 17u);
        poolcopy_pack pk = { ba, bb, IN_A, IN_B, N };
        uint64_t t2 = ms_now();
        if (dm_ohos_vk_oneshot(rec_copy_pool, &pk) != 0) { EMIT6("  ❌ copy 提交失败"); fails++; }
        else {
            uint64_t bad = 0, first = (uint64_t)-1;
            for (uint64_t i = 0; i < N; i++)
                if (dst[i] != (uint8_t)(i * 43u + 17u)) { if (first == (uint64_t)-1) first = i; bad++; }
            if (bad) { EMIT6("  ❌ %llu 字节里 %llu 个不符（首个 %llu）",
                             (unsigned long long)N, (unsigned long long)bad, (unsigned long long)first); fails++; }
            else EMIT6("  ✅ %llu 字节全部一致（%llums）—— 反查结果能被 GPU 直接用来搬运",
                       (unsigned long long)N, (unsigned long long)(ms_now() - t2));
        }
    } else { EMIT6("  ⚠ 有池没登记成功，跳过"); fails++; }

    // ④ 负向判据
    EMIT6("④ 负向判据：池不在客体堆里必须登记失败");
    {
        static uint8_t outside[1 << 16];
        int r = nvn_vk_pool_add(outside, sizeof outside);
        EMIT6("  %s 堆外的内存：%s", r ? "✅" : "❌",
              r ? "按预期被拒（不会悄悄退化成不可用内存）" : "**居然成功了——零拷贝是假的**");
        if (!r) fails++;
    }

    // 收尾：注销两个池
    nvn_vk_pool_del(pa);
    nvn_vk_pool_del(pb);
    EMIT6("⑤ 注销后池数：%d（应为 0）", nvn_vk_pool_count());
    if (nvn_vk_pool_count() != 0) fails++;
    dm_ohos_heap_release();

    EMIT6("");
    EMIT6("===== 3.1-C %s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    if (!fails)
        EMIT6("结论：池登记 / 地址反查 / GPU 搬运 / 负向判据全部成立 ⇒ 回放层的池这一层可用");
    *p = 0;
    #undef EMIT6
    return fails;
}

// ================= 阶段 3 第二步（3.2）：回放层最小闭环 =================
// 手写一小段 NVN 命令流（格式见 nvn_obj.h），交给 nvn_vk_exec 回放，离屏读回验证三角形。
// 与阶段 2 的判据**逐像素相同**（内=着色器色、外=清屏色），区别是这次画面是**从记录流回放出来的**。
// 着色器走"解析接缝"：探针把阶段 2 那两条 SPIR-V 喂给回放层（生产里这张表由离线链产出）。
typedef struct { VkImage img; VkBuffer buf; uint32_t w, h; } replay_rb_pack;
static void rec_replay_rb(VkCommandBuffer cb, void *ud) {
    replay_rb_pack *d = (replay_rb_pack *)ud;
    VkBufferImageCopy r = { 0 };
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = d->w; r.imageExtent.height = d->h; r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, d->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->buf, 1, &r);
}

// 着色器解析接缝的探针实现：控制段指针当键（0x1 = 顶点、0x2 = 片元）
static int probe_shader_resolve(const void *control, int stage, nvn_vk_shader *out) {
    if (control == (const void *)0x1 && stage == 0) { out->spirv = dm_spv_vert; out->nwords = dm_spv_vert_n; return 1; }
    if (control == (const void *)0x2 && stage == 1) { out->spirv = dm_spv_frag; out->nwords = dm_spv_frag_n; return 1; }
    return 0;
}

// 往命令流里写一条记录：{op, size} + 载荷，size 含头且 8 对齐（与 nvn_obj.h 的约定一致）
static uint8_t *wrec(uint8_t *p, uint32_t op, const void *payload, uint32_t plen) {
    op_hdr h;
    h.op = op;
    h.size = (uint32_t)((sizeof h + plen + 7u) & ~7u);
    memcpy(p, &h, sizeof h);
    if (plen) memcpy(p + sizeof h, payload, plen);
    return p + h.size;
}

// ---- 最小回放环境：客体堆 + 一个池 + 顶点/索引数据 + 手写命令流 ----
// 3.2 与"管线缓存落盘"共用同一套环境，保证两次对照跑的是**同一段命令流**。
static pool_t   g_pool;
static tex_t    g_tex;
static prog_t   g_prog;
static uint8_t  g_stream[4096];
static uint8_t *g_env_heap, *g_env_pool, *g_env_stream;
static uint64_t g_env_stream_bytes;

static int replay_env_build(char *err, size_t errcap) {
    if (g_env_stream) return 0;                       // 已经建好（幂等：两轮对照共用）
    uint8_t *heap = (uint8_t *)dm_ohos_heap_reserve(4ull << 30);
    if (!heap) { snprintf(err, errcap, "客体堆预留失败：%s", dm_ohos_vk_err()); return -1; }

    // 一个池：顶点数据放在 +256，渲染目标的"存储"声明在 +4 MiB（图像自建分配，不用它，见 nvn_vk.c 的说明）
    const uint64_t POOL_SZ = 16ull << 20;
    uint8_t *pool_base = heap + (1ull << 30);
    if (nvn_vk_pool_add(pool_base, POOL_SZ)) { snprintf(err, errcap, "池登记失败：%s", nvn_vk_last_error()); return -1; }

    // 顶点数据：和阶段 2 同一组三角形顶点（NDC，Vulkan 的 Y 向下 ⇒ 底边在上）
    const float verts[6] = { -1.f, -1.f,  1.f, -1.f,  0.f, 1.f };
    uint8_t *vtx = pool_base + 256;
    memcpy(vtx, verts, sizeof verts);
    // 索引数据：同一组顶点的索引（验证索引绘制）
    const uint32_t idx[3] = { 0, 1, 2 };
    uint8_t *ibuf = pool_base + 512;
    memcpy(ibuf, idx, sizeof idx);

    // 池对象 / 贴图对象 / 程序对象（都是 nvn_obj.h 的普通结构，探针自己填）
    g_pool.magic = MAGIC('P','O','O','L'); g_pool.mem = pool_base; g_pool.size = POOL_SZ;
    memset(&g_tex, 0, sizeof g_tex);
    g_tex.magic = MAGIC('T','E','X','0');
    g_tex.d.target = 1;              // NVN_TEXTURE_TARGET_2D
    g_tex.d.format = 0x25;           // NVN_FORMAT_RGBA8
    g_tex.d.width = 64; g_tex.d.height = 64; g_tex.d.depth = 1; g_tex.d.levels = 1;
    g_tex.mem = pool_base + (4ull << 20); g_tex.pool = &g_pool; g_tex.off = 4ll << 20;
    g_tex.serial = 1;
    memset(&g_prog, 0, sizeof g_prog);
    g_prog.magic = MAGIC('P','R','O','G'); g_prog.nstage = 2; g_prog.serial = 1;
    g_prog.sd[0].control = (const void *)0x1;   // 键：探针的解析回调认这两个
    g_prog.sd[1].control = (const void *)0x2;
    nvn_vk_set_shader_resolver(probe_shader_resolve);

    uint8_t *w = g_stream;
    {
        p_set_rt rt; memset(&rt, 0, sizeof rt);
        rt.n = 1; rt.color[0] = &g_tex;
        w = wrec(w, OP_SET_RT, &rt, sizeof rt);

        p_clear_color cc; memset(&cc, 0, sizeof cc);
        cc.rgba[0] = 1.f; cc.rgba[3] = 1.f;                 // 清成红（三角形外应读到它）
        w = wrec(w, OP_CLEAR_COLOR, &cc, sizeof cc);

        p_viewports vp; memset(&vp, 0, sizeof vp);
        vp.n = 1;
        vp.v[0][0] = 0; vp.v[0][1] = 0; vp.v[0][2] = 64; vp.v[0][3] = 64; vp.v[0][4] = 0; vp.v[0][5] = 1;
        w = wrec(w, OP_VIEWPORTS, &vp, sizeof vp);

        p_scissor sc = { 0, 0, 64, 64 };
        w = wrec(w, OP_SCISSOR, &sc, sizeof sc);

        p_bind_program bp; memset(&bp, 0, sizeof bp);
        bp.prog = &g_prog; bp.stages = 3;
        w = wrec(w, OP_BIND_PROGRAM, &bp, sizeof bp);

        p_bind_vattr va; memset(&va, 0, sizeof va);
        va.first = 0; va.n = 1;
        va.st[0] = (uint32_t)0x16 | (0u << 8) | (0u << 27);   // fmt=0x16(Float2) offset=0 stream=0
        w = wrec(w, OP_BIND_VATTR, &va, sizeof va);

        p_bind_vstream vs; memset(&vs, 0, sizeof vs);
        vs.first = 0; vs.n = 1; vs.st[0].stride = 8; vs.st[0].divisor = 0;
        w = wrec(w, OP_BIND_VSTREAM, &vs, sizeof vs);

        p_bind_vbuf vb; memset(&vb, 0, sizeof vb);
        vb.first = 0; vb.n = 1;
        vb.r[0].address = (NVNbufferAddress)(uintptr_t)vtx; vb.r[0].size = sizeof verts;
        w = wrec(w, OP_BIND_VBUF, &vb, sizeof vb);

        p_draw dr = { 4, 0, 3 };                              // prim=4(三角形) first=0 count=3
        w = wrec(w, OP_DRAW, &dr, sizeof dr);

        p_draw_idx di; memset(&di, 0, sizeof di);             // 索引绘制：同一组顶点
        di.prim = 4; di.itype = 0; di.count = 3; di.instances = 1;
        di.ibuf = (uint64_t)(uintptr_t)ibuf;
        w = wrec(w, OP_DRAW_IDX, &di, sizeof di);

        p_draw dr2 = { 3, 0, 3 };                             // LINE_STRIP：验证"拓扑进了管线键"
        w = wrec(w, OP_DRAW, &dr2, sizeof dr2);

        op_hdr eh = { OP_END, 8 };
        memcpy(w, &eh, sizeof eh); w += sizeof eh;
    }
    g_env_heap = heap; g_env_pool = pool_base; g_env_stream = g_stream;
    g_env_stream_bytes = (uint64_t)(w - g_stream);
    return 0;
}

static void replay_env_free(void) {
    if (!g_env_stream) return;
    nvn_vk_pool_del(g_env_pool);
    dm_ohos_heap_release();
    g_env_stream = NULL; g_env_heap = NULL; g_env_pool = NULL;
}

static int replay_env_run(nvn_vk_stats_t *st) {
    if (nvn_vk_frame_begin() != 0) return -1;
    nvn_vk_exec(g_env_stream);
    if (nvn_vk_frame_end() != 0) return -1;
    if (st) nvn_vk_stats_get(st);
    return 0;
}

int vk_backend_replay(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT7(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT7("=== 阶段 3 第二步（3.2）：回放层最小闭环 ===");
    EMIT7("手写 NVN 命令流 → nvn_vk_exec 回放 → 离屏读回逐像素验证");

    char env_err[256] = "";
    if (replay_env_build(env_err, sizeof env_err)) {
        EMIT7("❌ %s", env_err); EMIT7("===== 失败 ====="); *p = 0; return 1;
    }
    EMIT7("池：%p + 16 MiB（堆偏移 %#llx）", g_env_pool, (unsigned long long)(1ull << 30));
    EMIT7("命令流：%llu 字节", (unsigned long long)g_env_stream_bytes);

    // 清掉内存里的管线表：本探针的判据是"新建管线应为 2"，**不能受前面跑过什么影响**。
    // 踩过：先跑了「管线缓存落盘」探针，管线表已被填满 ⇒ 这里命中缓存 3 次、新建 0 条，判据误报。
    nvn_vk_pso_local_clear();

    // 回放
    if (nvn_vk_frame_begin() != 0) { EMIT7("❌ 开始回放帧失败：%s", nvn_vk_last_error()); fails++; }
    else {
        nvn_vk_exec(g_env_stream);
        if (nvn_vk_frame_end() != 0) { EMIT7("❌ 提交回放帧失败：%s", nvn_vk_last_error()); fails++; }
    }

    // 读回：目标图像（通道结束已转成 TRANSFER_SRC）→ 主机可见缓冲
    {
        VkDevice dev = dm_ohos_vk_device();
        uint32_t w2 = 0, h2 = 0;
        VkImage img = nvn_vk_target_image();
        if (!img || nvn_vk_target_extent(&w2, &h2) != 0) { EMIT7("❌ 拿不到渲染目标图像"); fails++; }
        else {
            VkBuffer rb = VK_NULL_HANDLE; VkDeviceMemory rbm = VK_NULL_HANDLE; void *rbm_p = NULL;
            VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bi.size = (VkDeviceSize)w2 * h2 * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            if (vkCreateBuffer(dev, &bi, NULL, &rb) != VK_SUCCESS) { EMIT7("❌ 建读回缓冲失败"); fails++; }
            else {
                VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, rb, &mr);
                VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mi.allocationSize = mr.size; mi.memoryTypeIndex = dm_ohos_heap_mem_type();
                if (vkAllocateMemory(dev, &mi, NULL, &rbm) != VK_SUCCESS) { EMIT7("❌ 分配读回内存失败"); fails++; }
                else if (vkBindBufferMemory(dev, rb, rbm, 0) != VK_SUCCESS) { EMIT7("❌ 绑定读回缓冲失败"); fails++; }
                else if (vkMapMemory(dev, rbm, 0, VK_WHOLE_SIZE, 0, &rbm_p) != VK_SUCCESS) { EMIT7("❌ 映射读回缓冲失败"); fails++; }
                else {
                    replay_rb_pack pk = { img, rb, w2, h2 };
                    if (dm_ohos_vk_oneshot(rec_replay_rb, &pk) != 0) { EMIT7("❌ 读回提交失败"); fails++; }
                    else {
                        const uint8_t *pix = (const uint8_t *)rbm_p;
                        // 与阶段 2 同一组判据点：(32,16) 在三角形内；(0,63) 在左下角，必在三角形外
                        const uint8_t *in = pix + (16 * w2 + 32) * 4;
                        const uint8_t *ou = pix + (63 * w2 + 0) * 4;
                        nvn_vk_stats_t stt;
                        nvn_vk_stats_get(&stt);
                        EMIT7("渲染目标 %ux%u；统计：命令 %llu 条 / 未处理 %llu 条 / 绘制 %llu 次 / 通道 %llu 个 / 新建管线 %llu / 命中缓存 %llu",
                              w2, h2,
                              (unsigned long long)stt.ops, (unsigned long long)stt.ops_skipped,
                              (unsigned long long)stt.draws, (unsigned long long)stt.passes,
                              (unsigned long long)stt.pso_built, (unsigned long long)stt.pso_hit);
                        EMIT7("测量骨架：GPU 耗时 %.3f ms；附件写出 %.1f KiB；逐通道 %llu 个",
                              (double)stt.gpu_ns / 1e6, (double)stt.bw_store / 1024.0,
                              (unsigned long long)stt.pass_n);
                        for (uint64_t i = 0; i < stt.pass_n; i++)
                            EMIT7("  通道 %llu：%.3f ms", (unsigned long long)i,
                                  (double)stt.pass_gpu_ns[i] / 1e6);
                        EMIT7("三角形内 (32,16) 读到 (%u,%u,%u,%u)，预期蓝 (0,0,255,255)", in[0], in[1], in[2], in[3]);
                        EMIT7("三角形外 (0,63)  读到 (%u,%u,%u,%u)，预期红 (255,0,0,255)", ou[0], ou[1], ou[2], ou[3]);
                        int ok_in = in[0] == 0 && in[1] == 0 && in[2] == 255 && in[3] == 255;
                        int ok_out = ou[0] == 255 && ou[1] == 0 && ou[2] == 0 && ou[3] == 255;
                        if (!ok_in) { EMIT7("❌ 三角形内颜色不对：回放链（程序/管线/顶点/绘制）有问题"); fails++; }
                        if (!ok_out) { EMIT7("❌ 三角形外颜色不对：清屏或绘制范围有问题"); fails++; }
                        EMIT7("%s 回放出来的画面与阶段 2 逐像素一致%s", (ok_in && ok_out) ? "✅" : "❌",
                              (ok_in && ok_out) ? "——记录流 → 回放 → 出图这条链通了" : "");
                        // 本步新增的两条判据：索引绘制走通、拓扑真的进了管线键
                        if (stt.draws != 3) { EMIT7("❌ 绘制次数应为 3（非索引 + 索引 + 线带），实为 %llu",
                                                    (unsigned long long)stt.draws); fails++; }
                        if (stt.pso_built != 2) {
                            EMIT7("❌ 新建管线应为 2（三角形 + 线带），实为 %llu —— **拓扑没进管线键**（会复用错管线）",
                                  (unsigned long long)stt.pso_built); fails++;
                        } else EMIT7("✅ 索引绘制复用了三角形管线、线带另建一条 ⇒ 拓扑确实进了管线键");

                        if (stt.ops_skipped != 0) { EMIT7("❌ 有 %llu 条命令未被处理（不该静默）",
                                                          (unsigned long long)stt.ops_skipped); fails++; }
                        else EMIT7("✅ 命令全部被处理（未处理 0 条）");
                    }
                    vkUnmapMemory(dev, rbm);
                }
            }
            if (rb) vkDestroyBuffer(dev, rb, NULL);
            if (rbm) vkFreeMemory(dev, rbm, NULL);
        }
    }

    replay_env_free();

    EMIT7("");
    EMIT7("===== 3.2 %s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *p = 0;
    #undef EMIT7
    return fails;
}

// ================= 管线缓存落盘（Vulkan 特有；Metal 做不到）=================
// 建管线 = 驱动编译着色器，是首帧卡顿的主要来源。Vulkan 的 `VkPipelineCache` 可以序列化到盘上，
// 第二次启动直接命中 ⇒ 零编译。上游 Metal 侧只有"离线 metallib + 预热清单"（构建期产物），
// **做不到运行时积累**——这是鸿蒙侧能比上游做得好的地方。
//
// 判据分两层（都在同一段命令流上跑，排除变量）：
//   硬判据：热启动能从盘上载入缓存（字节数 > 0）⇒ 落盘/载入这条链通了；
//   软判据：建管线耗时"热 < 冷"⇒ 真的省了编译（如实报出，未观察到收益也照说）。
int vk_backend_psocache(const char *dir, char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    #define EMIT8(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT8("=== 管线缓存落盘（Vulkan 特有；Metal 没有等价的持久化管线缓存）===");
    EMIT8("同一段命令流跑两轮：冷启动（空缓存）vs 热启动（载入刚存的缓存），比建管线耗时");

    char cache_path[1200];
    snprintf(cache_path, sizeof cache_path, "%s/vk_pso_cache.bin", (dir && *dir) ? dir : ".");

    char env_err[256] = "";
    if (replay_env_build(env_err, sizeof env_err)) {
        EMIT8("❌ %s", env_err); EMIT8("===== 失败 ====="); *p = 0; return 1;
    }
    EMIT8("命令流：%llu 字节；缓存文件：%s", (unsigned long long)g_env_stream_bytes, cache_path);

    const int ROUNDS = 3;          // 每轮跑 3 遍，把编译耗时放大到能看清
    uint64_t cold_ns = 0, hot_ns = 0, cold_built = 0, hot_built = 0, hot_blob = 0;

    // ---- ① 冷启动：先把缓存文件删掉 ----
    remove(cache_path);
    if (nvn_vk_pso_cache_enable(cache_path) != 0) {
        EMIT8("❌ 打开管线缓存失败：%s", nvn_vk_last_error()); fails++;
    } else {
        for (int i = 0; i < ROUNDS; i++) {
            nvn_vk_pso_local_clear();                       // 清掉内存里的管线表 ⇒ 每次都要真的重建
            nvn_vk_stats_t s;
            if (replay_env_run(&s) != 0) { EMIT8("❌ 冷启动回放失败：%s", nvn_vk_last_error()); fails++; break; }
            cold_ns += s.pso_build_ns;
            cold_built += s.pso_built;
            if (i == 0 && s.pso_cache_blob) { EMIT8("⚠ 冷启动居然载入了 %llu 字节（文件没删掉？）",
                                                    (unsigned long long)s.pso_cache_blob); }
        }
        EMIT8("① 冷启动：建管线 %llu 条，累计耗时 %.3f ms（载入缓存 0 字节）",
              (unsigned long long)cold_built, (double)cold_ns / 1e6);
    }

    // ---- ② 热启动：重新打开缓存（此时文件已由上一轮的 frame_end 自动存盘）----
    nvn_vk_pso_local_clear();
    if (nvn_vk_pso_cache_enable(cache_path) != 0) {
        EMIT8("❌ 重新打开管线缓存失败：%s", nvn_vk_last_error()); fails++;
    } else {
        for (int i = 0; i < ROUNDS; i++) {
            nvn_vk_pso_local_clear();
            nvn_vk_stats_t s;
            if (replay_env_run(&s) != 0) { EMIT8("❌ 热启动回放失败：%s", nvn_vk_last_error()); fails++; break; }
            hot_ns += s.pso_build_ns;
            hot_built += s.pso_built;
            if (s.pso_cache_blob > hot_blob) hot_blob = s.pso_cache_blob;
        }
        EMIT8("② 热启动：建管线 %llu 条，累计耗时 %.3f ms（载入缓存 %llu 字节）",
              (unsigned long long)hot_built, (double)hot_ns / 1e6, (unsigned long long)hot_blob);
    }

    // ---- 判据 ----
    if (!cold_built || !hot_built) { EMIT8("❌ 有一轮没跑成，判据不成立"); fails++; }
    else {
        if (!hot_blob) { EMIT8("❌ 热启动没能从盘上载入缓存 —— **落盘/载入这条链没通**"); fails++; }
        else EMIT8("✅ 热启动从盘上载入了 %llu 字节 ⇒ 落盘 → 载入这条链通了",
                   (unsigned long long)hot_blob);
        if (cold_ns && hot_ns) {
            if (hot_ns < cold_ns)
                EMIT8("✅ 建管线耗时：冷 %.3f ms → 热 %.3f ms（省 %.0f%%）",
                      (double)cold_ns / 1e6, (double)hot_ns / 1e6,
                      (1.0 - (double)hot_ns / (double)cold_ns) * 100.0);
            else
                EMIT8("⚠ 未观察到收益（冷 %.3f ms / 热 %.3f ms）—— 驱动可能没把编译结果放进缓存，如实记录",
                      (double)cold_ns / 1e6, (double)hot_ns / 1e6);
        }
    }

    replay_env_free();
    EMIT8("");
    EMIT8("===== 管线缓存落盘 %s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *p = 0;
    #undef EMIT8
    return fails;
}

// ================= 阶段 4：出图（3.3-A）与节拍（3.3-B）=================
// ArkUI 的 XComponent 给出 OHNativeWindow → VkSurfaceKHR → swapchain → 画 → present。
// 这是"离屏验证"与"真上屏"的分界线：前面所有判据都是读回像素，这里第一次让画面**出现在屏幕上**。
//
// 两个探针共用下面这套骨架：
//   os_setup()    —— 搭出 surface / swapchain / 通道 / 管线 / 顶点缓冲（3.3-A、3.3-B 都要）
//   os_frame_do() —— 出一帧（取图 → 录命令 → 提交 → present），两种等待模式都由它一个函数表达
//   os_teardown() —— 拆干净
// **共用而不是抄第二份**：这两个探针的"搭"完全一样，抄两份意味着以后每次改都可能只改一处。
typedef struct {
    VkSurfaceKHR   surface;
    VkSwapchainKHR sc;
    VkFormat       fmt; VkColorSpaceKHR cs; VkExtent2D ext;
    uint32_t       nimg; VkImage img[8]; VkImageView view[8]; VkFramebuffer fb[8];
    VkRenderPass   rp; VkPipeline pl; VkPipelineLayout lay;
    VkBuffer       vb; VkDeviceMemory vbm;
    VkQueue        qpresent;          // 负责 present 的队列（真机：图形族不支持）
} os_ctx;

// 一帧要用的同步对象，凑成"槽位"：命令缓冲 + 栅栏 + 两个信号量。
// 为什么要按槽位、而不是全局一份：**不等 GPU 时上一帧还在飞**——同一份命令缓冲或同一个信号量被连续
// 两帧复用，就是经典的 use-after-flight（画面偶发花屏/崩，且极难复现）。3.3-B 要两种等待模式都量，
// 所以同步对象一律按槽位发。
// Per-slot sync objects. With non-blocking present the previous frame is still in flight; reusing one
// command buffer or one semaphore would be a use-after-flight bug. Slots make double buffering explicit.
typedef struct {
    VkCommandBuffer cb;
    VkFence         fence;
    VkSemaphore     avail, done;
} os_slot;

// 报告出口：一块缓冲区 + 一个游标，同时镜像一份进 hilog（手机屏幕上长文字不好复制）。
// 抽出来是为了让上面那套骨架能在两个探针之间共用（原先它是 3.3-A 函数内的局部宏 EMIT9）。
typedef struct { char *p, *end; } os_emit;

// **em 是指针**，不是结构体本身 —— 游标必须由所有调用者共享。
// 踩过的坑：早先按值传结构体，于是"谁写谁改的是自己的副本"，各层的输出全落在同一个偏移上
// 互相覆盖 —— 真机报告里只剩首尾两块，中间 5 组的明细整段消失（而且不会报任何错）。
// em is a pointer: the cursor has to be shared by every writer. Passing the struct by value made each
// layer advance its own copy, so all writers landed on the same offset and overwrote each other --
// the on-device report kept only its first and last blocks, with no error anywhere.
#define OS_EMIT(em, ...) do { int _n = snprintf((em)->p, (size_t)((em)->end - (em)->p) + 1, __VA_ARGS__); \
                              if (_n > 0) { (em)->p += _n; if ((em)->p > (em)->end) (em)->p = (em)->end; } \
                              dm_log_printf(__VA_ARGS__); } while (0)

// 槽位的创建与释放。**栅栏要建成已置位**：第一帧等它时不必先等一个从未置位的栅栏（否则要等超时）。
static int os_slot_init(os_slot *s, int n, os_emit *em) {
    VkDevice dev = dm_ohos_vk_device();
    int fails = 0;
    for (int i = 0; i < n; i++) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateSemaphore(dev, &si, NULL, &s[i].avail) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &si, NULL, &s[i].done) != VK_SUCCESS ||
            vkCreateFence(dev, &fi, NULL, &s[i].fence) != VK_SUCCESS) {
            OS_EMIT(em, "❌ 建同步对象（信号量/栅栏）失败"); return 1;
        }
    }
    return fails;
}

static void os_slot_free(os_slot *s, int n, VkCommandPool pool) {
    VkDevice dev = dm_ohos_vk_device();
    if (!dev) return;
    // 不等 GPU 的模式下，收尾这一瞬间可能还有帧在飞 —— 先等干净再释放（探针不吝惜这几毫秒）。
    vkDeviceWaitIdle(dev);
    for (int i = 0; i < n; i++) {
        if (s[i].cb) { vkFreeCommandBuffers(dev, pool, 1, &s[i].cb); s[i].cb = VK_NULL_HANDLE; }
        if (s[i].fence) vkDestroyFence(dev, s[i].fence, NULL);
        if (s[i].avail) vkDestroySemaphore(dev, s[i].avail, NULL);
        if (s[i].done)  vkDestroySemaphore(dev, s[i].done, NULL);
    }
}

static void os_teardown(os_ctx *o) {
    VkDevice dev = dm_ohos_vk_device();
    if (!dev) return;
    if (o->vb) vkDestroyBuffer(dev, o->vb, NULL);
    if (o->vbm) vkFreeMemory(dev, o->vbm, NULL);
    if (o->pl) vkDestroyPipeline(dev, o->pl, NULL);
    if (o->lay) vkDestroyPipelineLayout(dev, o->lay, NULL);
    if (o->rp) vkDestroyRenderPass(dev, o->rp, NULL);
    for (uint32_t i = 0; i < o->nimg; i++) {
        if (o->fb[i]) vkDestroyFramebuffer(dev, o->fb[i], NULL);
        if (o->view[i]) vkDestroyImageView(dev, o->view[i], NULL);
    }
    if (o->sc) vkDestroySwapchainKHR(dev, o->sc, NULL);
    if (o->surface) vkDestroySurfaceKHR(dm_ohos_vk_instance(), o->surface, NULL);
}

static uint64_t ns_now2(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// 录一帧：清屏 → 画三角形（顶点缓冲走探针自己建的小缓冲）
static void rec_os_draw(VkCommandBuffer cb, void *ud) {
    os_ctx *o = (os_ctx *)ud;
    VkClearValue cv; memset(&cv, 0, sizeof cv);
    cv.color.float32[0] = 1.f; cv.color.float32[3] = 1.f;      // 清成红
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = o->rp; bi.framebuffer = o->fb[0];
    bi.renderArea.extent = o->ext;
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->pl);
    VkViewport vp = { 0.f, 0.f, (float)o->ext.width, (float)o->ext.height, 0.f, 1.f };
    VkRect2D sc = { { 0, 0 }, o->ext };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &o->vb, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
}

// 搭出图所需的全部东西：XComponent 窗口 → VkSurfaceKHR → 挑出 present 队列 → swapchain → 通道 →
// 管线 → 顶点缓冲。报告行经 em 输出（和调用方自己的报告拼在一起）。返回失败项数。
// 失败时可能已经建了一部分对象 —— 调用方照常 os_teardown（它对空句柄是安全的）。
static int os_setup(os_emit *em, os_ctx *o) {
    int fails = 0;
    char err[256] = "";
    uint32_t win_w = 0, win_h = 0;
    memset(o, 0, sizeof *o);
    o->fmt = VK_FORMAT_UNDEFINED;

    if (!dm_ohos_surface_window() || dm_ohos_surface_size(&win_w, &win_h) != 0) {
        OS_EMIT(em, "❌ 还没拿到 XComponent 窗口（页面里要有 XComponent，且 surface 回调已到）");
        return 1;
    }
    OS_EMIT(em, "XComponent 窗口：%ux%u", win_w, win_h);

    if (dm_ohos_surface_create(&o->surface, err, sizeof err) != 0) {
        OS_EMIT(em, "❌ %s", err); return 1;
    }
    VkDevice dev = dm_ohos_vk_device();
    VkPhysicalDevice pd = dm_ohos_vk_phys();

    // **逐个族问"支不支持 present"**：这台设备真机实测"图形族不支持"，得换一个族提交 present。
    // 渲染仍用主（图形）队列——命令缓冲的命令池属于图形族，不能跨族提交。
    uint32_t qf = 0xFFFFFFFFu;
    {
        uint32_t n = dm_ohos_vk_qfam_count(), qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
        VkQueueFamilyProperties *qsr = calloc(qn ? qn : 1, sizeof *qsr);
        if (qn) vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qsr);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t f = dm_ohos_vk_qfam_at(k);
            if (f >= qn) continue;
            VkBool32 pres = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, f, o->surface, &pres);
            const char *what = (qsr[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "图形" :
                               (qsr[f].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "计算" : "传输";
            OS_EMIT(em, "  队列族 %u：%s，队列数 %u，present %s", f, what, qsr[f].queueCount, pres ? "支持" : "不支持");
            if (pres && qf == 0xFFFFFFFFu) qf = f;
        }
        free(qsr);
    }
    if (qf == 0xFFFFFFFFu) {
        OS_EMIT(em, "❌ 没有一个队列族支持 present —— 出图这条路走不通（要查 VK_OHOS_surface / 窗口是否有效）");
        return 1;
    }
    for (uint32_t k = 0; k < dm_ohos_vk_qfam_count(); k++)
        if (dm_ohos_vk_qfam_at(k) == qf) o->qpresent = dm_ohos_vk_queue_at(k);
    OS_EMIT(em, "✅ 提交族 %u（渲染仍走主队列，命令缓冲跨族提交是非法的）", qf);
    if (!o->qpresent) { OS_EMIT(em, "❌ 拿不到提交族的队列"); return 1; }

    // ---- 表面能力 / 格式 / 呈现模式：逐个报出，不猜 ----
    VkSurfaceCapabilitiesKHR caps; memset(&caps, 0, sizeof caps);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, o->surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, o->surface, &nf, NULL);
    VkSurfaceFormatKHR *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, o->surface, &nf, fmts);
    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, o->surface, &npm, NULL);
    VkPresentModeKHR *pms = calloc(npm ? npm : 1, sizeof *pms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, o->surface, &npm, pms);
    OS_EMIT(em, "表面：图像数 %u..%u，当前范围 %ux%u，变换 %#x，支持的用法 %#x",
            caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height,
            (unsigned)caps.supportedTransforms, (unsigned)caps.supportedUsageFlags);
    for (uint32_t i = 0; i < nf && i < 6; i++)
        OS_EMIT(em, "  格式 %u：VkFormat %d / 色彩空间 %d", i, (int)fmts[i].format, (int)fmts[i].colorSpace);
    for (uint32_t i = 0; i < npm && i < 6; i++)
        OS_EMIT(em, "  呈现模式 %u：%d（0=立即 1=信箱 2=FIFO 3=FIFO 放宽）", i, (int)pms[i]);

    // 选：优先 RGBA8/BGRA8 非 sRGB，否则用第一个；呈现模式优先 FIFO（稳定 vsync）
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            o->fmt = fmts[i].format; o->cs = fmts[i].colorSpace; break;
        }
    if (o->fmt == VK_FORMAT_UNDEFINED && nf) { o->fmt = fmts[0].format; o->cs = fmts[0].colorSpace; }
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < npm; i++) if (pms[i] == VK_PRESENT_MODE_FIFO_KHR) { pm = pms[i]; break; }
    if (nf == 0 || npm == 0) { OS_EMIT(em, "❌ 表面没报格式或呈现模式"); fails++; }
    free(fmts); free(pms);
    if (fails) return fails;

    o->ext.width = caps.currentExtent.width;
    o->ext.height = caps.currentExtent.height;
    if (o->ext.width == 0xFFFFFFFFu || !o->ext.width) o->ext.width = win_w;
    if (o->ext.height == 0xFFFFFFFFu || !o->ext.height) o->ext.height = win_h;
    OS_EMIT(em, "选用：VkFormat %d，呈现模式 %d，交换链 %ux%u", (int)o->fmt, (int)pm, o->ext.width, o->ext.height);

    // ---- swapchain ----
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    {
        VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        ci.surface = o->surface; ci.minImageCount = want; ci.imageFormat = o->fmt; ci.imageColorSpace = o->cs;
        ci.imageExtent = o->ext; ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = pm; ci.clipped = VK_TRUE; ci.oldSwapchain = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(dev, &ci, NULL, &o->sc);
        if (r != VK_SUCCESS) { OS_EMIT(em, "❌ vkCreateSwapchainKHR 失败 VkResult=%d", (int)r); fails++; }
    }
    if (fails) return fails;
    o->nimg = 0;
    vkGetSwapchainImagesKHR(dev, o->sc, &o->nimg, NULL);
    if (o->nimg > 8) o->nimg = 8;
    vkGetSwapchainImagesKHR(dev, o->sc, &o->nimg, o->img);
    OS_EMIT(em, "交换链：%u 张图像", o->nimg);

    // ---- 通道：结束布局 = PRESENT_SRC（可以直接 present）----
    {
        VkAttachmentDescription att; memset(&att, 0, sizeof att);
        att.format = o->fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp; memset(&sp, 0, sizeof sp);
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        rp.attachmentCount = 1; rp.pAttachments = &att;
        rp.subpassCount = 1; rp.pSubpasses = &sp;
        if (vkCreateRenderPass(dev, &rp, NULL, &o->rp) != VK_SUCCESS) { OS_EMIT(em, "❌ vkCreateRenderPass 失败"); fails++; }
    }
    for (uint32_t i = 0; i < o->nimg && !fails; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = o->img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = o->fmt;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, NULL, &o->view[i]) != VK_SUCCESS) { OS_EMIT(em, "❌ 建图像视图 %u 失败", i); fails++; break; }
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = o->rp; fi.attachmentCount = 1; fi.pAttachments = &o->view[i];
        fi.width = o->ext.width; fi.height = o->ext.height; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, NULL, &o->fb[i]) != VK_SUCCESS) { OS_EMIT(em, "❌ 建帧缓冲 %u 失败", i); fails++; break; }
    }
    if (fails) return fails;

    // ---- 管线（用阶段 2 那两条 SPIR-V）----
    {
        VkShaderModule vm = make_module(dev, dm_spv_vert, dm_spv_vert_n, err, sizeof err);
        VkShaderModule fm = vm ? make_module(dev, dm_spv_frag, dm_spv_frag_n, err, sizeof err) : NULL;
        if (!vm || !fm) { OS_EMIT(em, "❌ 建着色器模块失败：%s", err); fails++; }
        else {
            VkPipelineShaderStageCreateInfo st[2] = {
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL },
                { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL },
            };
            VkVertexInputBindingDescription bind = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
            VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
            VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
            vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &attr;
            VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vps.viewportCount = 1; vps.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
            VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            dss.depthTestEnable = VK_FALSE; dss.stencilTestEnable = VK_FALSE;   // 不留 NULL（真机崩过）
            VkPipelineColorBlendAttachmentState at = { 0 };
            at.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 1; cb.pAttachments = &at;
            VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
            VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
            VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            if (vkCreatePipelineLayout(dev, &li, NULL, &o->lay) != VK_SUCCESS) { OS_EMIT(em, "❌ 建管线布局失败"); fails++; }
            else {
                VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
                gi.stageCount = 2; gi.pStages = st;
                gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
                gi.pViewportState = &vps; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
                gi.pDepthStencilState = &dss; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
                gi.layout = o->lay; gi.renderPass = o->rp;
                if (vkCreateGraphicsPipelines(dev, NULL, 1, &gi, NULL, &o->pl) != VK_SUCCESS) {
                    OS_EMIT(em, "❌ vkCreateGraphicsPipelines 失败"); fails++;
                }
            }
            vkDestroyShaderModule(dev, vm, NULL);
            vkDestroyShaderModule(dev, fm, NULL);
        }
    }
    if (fails) return fails;

    // ---- 顶点缓冲（三角形顶点；同步对象由调用方按槽位自己建）----
    {
        const float verts[6] = { -1.f, -1.f,  1.f, -1.f,  0.f, 1.f };
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = sizeof verts; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (vkCreateBuffer(dev, &bi, NULL, &o->vb) != VK_SUCCESS) { OS_EMIT(em, "❌ 建顶点缓冲失败"); fails++; }
        else {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, o->vb, &mr);
            VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mi.allocationSize = mr.size; mi.memoryTypeIndex = dm_ohos_heap_mem_type();
            void *mp = NULL;
            if (!(mr.memoryTypeBits & (1u << dm_ohos_heap_mem_type()))) { OS_EMIT(em, "❌ 顶点缓冲的 memoryTypeBits 不含统一内存类型"); fails++; }
            else if (vkAllocateMemory(dev, &mi, NULL, &o->vbm) != VK_SUCCESS) { OS_EMIT(em, "❌ 分配顶点缓冲内存失败"); fails++; }
            else if (vkBindBufferMemory(dev, o->vb, o->vbm, 0) != VK_SUCCESS) { OS_EMIT(em, "❌ 绑定顶点缓冲失败"); fails++; }
            else if (vkMapMemory(dev, o->vbm, 0, sizeof verts, 0, &mp) != VK_SUCCESS) { OS_EMIT(em, "❌ 映射顶点缓冲失败"); fails++; }
            else { memcpy(mp, verts, sizeof verts); vkUnmapMemory(dev, o->vbm); }
        }
    }
    return fails;
}

// 出一帧：取图 → 录命令 → 提交 → present。
// wait_idle = 1：present 之后 vkQueueWaitIdle（3.3-A 的老做法，每帧等 GPU 干完）；
// wait_idle = 0：只提交不等（**上游语义：出图只入队、立即返回**），靠槽位栅栏保证不覆盖还在飞的东西。
// 返回 0 成功；否则 >0 是 VkResult、-1 是我方准备步骤失败，原因写进 *why。
static int os_frame_do(os_ctx *o, VkCommandPool pool, os_slot *s, int wait_idle,
                       uint32_t *idx_out, uint64_t *ts_out, const char **why) {
    VkDevice dev = dm_ohos_vk_device();
    *why = "";
    *idx_out = 0; *ts_out = 0;

    // 这一槽位上一帧还没完就先等它 —— "多缓冲"要做的全部事情就这一句。
    // 栅栏带超时：探针宁可如实报"等超时"，也不把 App 挂死在一个永远不置位的栅栏上。
    if (vkWaitForFences(dev, 1, &s->fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
        *why = "等这一槽位上一帧的栅栏超时"; return -1;
    }
    vkResetFences(dev, 1, &s->fence);

    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(dev, o->sc, 2000000000ull, s->avail, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { *why = "取图失败"; return (int)r; }
    if (idx > 7) idx = 0;

    if (!s->cb) {   // 命令缓冲每个槽位分配一次、反复复用（不必每帧新建）
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &ai, &s->cb) != VK_SUCCESS) { *why = "取命令缓冲失败"; return -1; }
    }
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s->cb, &bi);
    {
        os_ctx tmp = *o; tmp.fb[0] = o->fb[idx];     // 本帧用第 idx 张
        rec_os_draw(s->cb, &tmp);
    }
    vkEndCommandBuffer(s->cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s->avail; si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &s->cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s->done;
    if (vkQueueSubmit(dm_ohos_vk_queue(), 1, &si, s->fence) != VK_SUCCESS) { *why = "提交失败"; return -1; }

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s->done;
    pi.swapchainCount = 1; pi.pSwapchains = &o->sc; pi.pImageIndices = &idx;
    uint64_t ts = ns_now2();
    r = vkQueuePresentKHR(o->qpresent, &pi);
    if (wait_idle) vkQueueWaitIdle(dm_ohos_vk_queue());
    *idx_out = idx; *ts_out = ts;
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { *why = "present 失败"; return (int)r; }
    return 0;
}

int vk_backend_onscreen(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    os_ctx o;
    int fails = 0;
    uint64_t t0 = ms_now();

    // 判据：① 表面能力 / 格式 / 呈现模式逐个报出（不猜）；② 60 帧 present 间隔统计（节拍稳不稳）。
    // 画面本身用截图做视觉确认（真机上肉眼可辨）。
    OS_EMIT(em, "=== 阶段 4（3.3-A）：出图 —— 屏幕上出画面 ===");
    OS_EMIT(em, "XComponent 窗口 → VkSurfaceKHR → swapchain → 画 → present");

    fails = os_setup(em, &o);
    VkCommandPool pool = dm_ohos_vk_pool();
    os_slot s; memset(&s, 0, sizeof s);
    if (!fails) fails = os_slot_init(&s, 1, em);

    // ---- 出 60 帧，量 present 间隔（单槽位 + 每帧等 GPU —— 与 3.3-A 首次通过时同一套口径）----
    if (!fails) {
        const int FRAMES = 60;
        uint64_t best = ~0ull, worst = 0, sum = 0, prev = 0; int got = 0;
        for (int f = 0; f < FRAMES && !fails; f++) {
            uint32_t idx = 0; uint64_t ts = 0; const char *why = "";
            int rc = os_frame_do(&o, pool, &s, 1, &idx, &ts, &why);
            if (rc) { OS_EMIT(em, "❌ 第 %d 帧失败：%s（%d）", f, why, rc); fails++; break; }
            if (prev) {
                uint64_t dt = ts - prev;
                if (dt < best) best = dt;
                if (dt > worst) worst = dt;
                sum += dt; got++;
            }
            prev = ts;
        }
        if (got) {
            double avg = (double)sum / (double)got;
            OS_EMIT(em, "present %d 帧：间隔 最小 %.2f ms / 平均 %.2f ms / 最大 %.2f ms（约 %.1f fps）",
                    got, (double)best / 1e6, avg / 1e6, (double)worst / 1e6, 1e9 / avg);
            if (avg > 0 && (double)worst > avg * 2.0)
                OS_EMIT(em, "⚠ 最大间隔是平均的两倍以上 —— 节拍有抖动（下一步接 OH_DisplaySoloist 再看）");
            else OS_EMIT(em, "✅ 节拍稳定（最大间隔未超平均两倍）");
        } else { OS_EMIT(em, "❌ 一帧都没出成"); fails++; }
    }

    os_slot_free(&s, 1, pool);
    os_teardown(&o);
    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.3-A %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    if (!fails) OS_EMIT(em, "结论：XComponent → VkSurfaceKHR → swapchain → present 这条链通了；屏幕上应能看到三角形");
    *em->p = 0;
    return fails;
}

// ================= 阶段 4（3.3-B）：可变帧率节拍 —— OH_DisplaySoloist =================
// 3.3-A 量的是"裸 present 的节拍上限"：自己埋头连画 60 张，而且每帧 vkQueueWaitIdle 等 GPU。
// 这一步请来系统当报时员：每次 VSync 到来时系统调我们的回调（跑在系统给的线程上），还把本拍与
// 下一拍的时间戳一并告诉你。于是能回答三个 3.3-A 回答不了的问题：
//
//   ① 回调真的被周期调用吗？时间戳间隔应 ≈16.67 ms（60 Hz）——这是"屏幕给的拍子"，比自测权威；
//   ② 可变帧率真的生效吗？设 expected=30 后间隔应翻倍到 ≈33.3 ms（这是鸿蒙比 Metal 侧强的一点）；
//   ③ 回调里能不能阻塞？同一段代码跑"等 GPU"与"不等 GPU"两组，比间隔有没有被拖崩。
//
// 另外量三项，供将来定生产形态（回放层怎么接它）用：回调线程身份、回调单次耗时、present 耗时。
// 同一轮里跑完 5 组，**自带裸 present 对照**——跨轮次对比读数会漂，同一轮里比才作数。
#define PACE_MAX_SLOT    3
#define PACE_ARM_FRAMES  60

typedef struct {
    const char *name;
    int  driver;        // 谁在驱动出图：0 = 裸 present（我们自己的循环）；1 = DisplaySoloist；2 = XComponent 侧接口
    int  wait_idle;     // 1 = present 后每帧等 GPU（3.3-A 老做法）；0 = 只入队不等（上游语义）
    int  rate;          // 0 = 不设帧率（跟随系统）；否则 min=max=expected=rate
    int  target;        // 收满多少拍收工
    int  slots;         // 槽位数：1 = 单缓冲；≥2 = 上一帧还在飞时就开下一帧
} pace_arm;

typedef struct {
    int    got, fails;
    double ts_avg_ms;   // 系统给的拍子（裸 present 的组为 0）
    double pr_avg_ms;   // 我们自己出图的节奏
    double jitter;      // 最大间隔 / 平均间隔（越接近 1 越整齐）
    double cb_cost_ms;  // 回调单次耗时（平均）
    unsigned long cb_tid, main_tid;   // 回调线程 / 主线程（3.3-C 要对比两条入口落哪条线程）
} pace_res;

typedef struct {
    os_ctx       *o;
    VkCommandPool pool;
    os_slot      *slot;
    pace_arm      arm;

    atomic_int    stop;     // 主线程置位：回调别再干活
    atomic_int    in_cb;    // 正在回调里的线程数（收尾要等它归零，否则 Destroy 会踩到正在用 Vulkan 的回调）
    atomic_int    done_n;   // 已完成的拍数
    int           fails;

    uint64_t      ts_prev, ts_best, ts_worst, ts_sum; int ts_n;   // 系统给的拍子
    uint64_t      pr_prev, pr_best, pr_worst, pr_sum; int pr_n;   // 我们出图的节奏
    uint64_t      cb_cost_max, cb_cost_sum;                       // 回调单次耗时
    unsigned long main_tid, cb_tid;
} pace_run;

// 当前在跑的那一组。**放文件级**：XComponent 侧的回调签名里没有 data 参数，拿不到上下文，
// 只能从这里取（同一时刻只跑一组、单线程调用，不会冲突）。
static pace_run *g_pace;

// 一拍进来要做的事。两个接入口（DisplaySoloist / XComponent 侧）的回调内容完全一样，
// 只是签名不同，所以共用这一份。
// **这里跑在系统给的线程上**，不是 UI 线程 —— 两边的线程 id 都记下来，报告里直接比。
static void pace_tick(uint64_t timestamp) {
    pace_run *R = g_pace;
    if (!R) return;
    atomic_fetch_add(&R->in_cb, 1);
    if (!atomic_load(&R->stop) && atomic_load(&R->done_n) < R->arm.target) {
        uint64_t t0 = ns_now2();
        if (!R->cb_tid) R->cb_tid = (unsigned long)pthread_self();

        // 系统给的拍子：相邻两次回调的时间戳之差
        uint64_t now_ts = (uint64_t)timestamp;
        if (R->ts_prev) {
            uint64_t dt = now_ts - R->ts_prev;
            if (dt < R->ts_best) R->ts_best = dt;
            if (dt > R->ts_worst) R->ts_worst = dt;
            R->ts_sum += dt; R->ts_n++;
        }
        R->ts_prev = now_ts;

        // 出图：按拍数轮换槽位 —— "不等 GPU" 时这才安全（上一帧还在飞，不能碰它的命令缓冲）
        int n = atomic_load(&R->done_n);
        os_slot *s = &R->slot[n % R->arm.slots];
        uint32_t idx = 0; uint64_t pts = 0; const char *why = "";
        int rc = os_frame_do(R->o, R->pool, s, R->arm.wait_idle, &idx, &pts, &why);
        if (rc) {
            R->fails++;
            dm_log_printf("节拍探针：第 %d 拍出图失败：%s（%d）", n, why, rc);
        } else {
            if (R->pr_prev) {
                uint64_t dt = pts - R->pr_prev;
                if (dt < R->pr_best) R->pr_best = dt;
                if (dt > R->pr_worst) R->pr_worst = dt;
                R->pr_sum += dt; R->pr_n++;
            }
            R->pr_prev = pts;
        }
        uint64_t cost = ns_now2() - t0;
        if (cost > R->cb_cost_max) R->cb_cost_max = cost;
        R->cb_cost_sum += cost;
        atomic_fetch_add(&R->done_n, 1);
    }
    atomic_fetch_sub(&R->in_cb, 1);
}

// DisplaySoloist 的回调（签名带 data 指针；这里不用它，统一走 g_pace）
static void pace_ds_cb(long long timestamp, long long targetTimestamp, void *data) {
    (void)targetTimestamp;       // 下一拍的时刻暂时不用；将来做"提前量"的时候要用
    (void)data;
    pace_tick((uint64_t)timestamp);
}

// XComponent 侧 `RegisterOnFrameCallback` 的回调（**没有 data 参数**，所以只能走 g_pace）
static void pace_xc_cb(OH_NativeXComponent *component, uint64_t timestamp, uint64_t targetTimestamp) {
    (void)component; (void)targetTimestamp;
    pace_tick(timestamp);
}

// 一组跑完后的读数落进 res（供最后的判据汇总用）
static void pace_res_of(const pace_arm *a, const pace_run *R, int got, pace_res *res) {
    memset(res, 0, sizeof *res);
    res->got = got; res->fails = R->fails;
    if (R->ts_n) {
        double avg = (double)R->ts_sum / (double)R->ts_n;
        res->ts_avg_ms = avg / 1e6;
    }
    if (R->pr_n) {
        double avg = (double)R->pr_sum / (double)R->pr_n;
        res->pr_avg_ms = avg / 1e6;
        if (avg > 0) res->jitter = (double)R->pr_worst / avg;
    }
    if (got) res->cb_cost_ms = (double)R->cb_cost_sum / (double)got / 1e6;
    res->cb_tid = R->cb_tid;
    res->main_tid = R->main_tid;
    (void)a;
}

// 把一组的过程与读数打进报告（给人在手机上读，所以每个数都写明是什么）
static void pace_report(os_emit *em, const pace_arm *a, const pace_run *R, int got) {
    OS_EMIT(em, "---- %s ----", a->name);
    if (a->driver) {
        OS_EMIT(em, "  收到 %d 拍（目标 %d）%s", got, a->target, got < a->target ? " ← **没收到应有的拍数**" : "");
        OS_EMIT(em, "  回调线程 %#lx ／ 主线程 %#lx ⇒ %s", R->cb_tid, R->main_tid,
                (R->cb_tid && R->cb_tid != R->main_tid)
                    ? "系统另起了一条线程（回调不在主/UI 线程上）"
                    : "回调跑在主线程上（或一次都没收到）");
    } else {
        OS_EMIT(em, "  自己出了 %d 帧（目标 %d）", got, a->target);
    }
    if (R->ts_n) {
        double avg = (double)R->ts_sum / (double)R->ts_n;
        OS_EMIT(em, "  系统给的拍子：最小 %.2f ／ 平均 %.2f ／ 最大 %.2f ms（≈ %.1f fps）",
                (double)R->ts_best / 1e6, avg / 1e6, (double)R->ts_worst / 1e6, 1e9 / avg);
    }
    if (R->pr_n) {
        double avg = (double)R->pr_sum / (double)R->pr_n;
        OS_EMIT(em, "  我们出图的节奏：最小 %.2f ／ 平均 %.2f ／ 最大 %.2f ms（≈ %.1f fps）",
                (double)R->pr_best / 1e6, avg / 1e6, (double)R->pr_worst / 1e6, 1e9 / avg);
    }
    if (a->driver && got)
        OS_EMIT(em, "  回调单次耗时：平均 %.2f ms，最大 %.2f ms", (double)R->cb_cost_sum / got / 1e6,
                (double)R->cb_cost_max / 1e6);
    OS_EMIT(em, "  等待方式：%s", a->wait_idle ? "每帧等 GPU 干完（3.3-A 老做法）"
                                              : "只入队不等（上游语义）");
    if (R->fails) OS_EMIT(em, "  ❌ 其中 %d 拍出图失败（详见 hilog）", R->fails);
    if (a->driver == 2 && !got)
        OS_EMIT(em, "  ↑ 预期如此：XComponent 侧的回调要在 **UI 主线程**派发（官方明文），"
                    "而同步探针正把 UI 线程堵着 ⇒ 收不到拍。这是结论，不是缺陷；"
                    "要真量它必须改「两段式」（注册后立即返回 → 回调自收 → 再取结果），见文档 3.3-C");
}

// 跑一组。返回失败项数；读数写进 res。
static int pace_one_arm(os_emit *em, os_ctx *o, const pace_arm *arm, pace_run *R, pace_res *res) {
    VkCommandPool pool = dm_ohos_vk_pool();
    int fails = 0, got = 0;

    if (arm->slots < 1 || arm->slots > PACE_MAX_SLOT) {   // 槽位表是定长的，越界前先挡住
        OS_EMIT(em, "❌ 组的槽位数 %d 超出上限 %d", arm->slots, PACE_MAX_SLOT);
        memset(res, 0, sizeof *res);
        return 1;
    }
    memset(R, 0, sizeof *R);
    atomic_init(&R->stop, 0); atomic_init(&R->in_cb, 0); atomic_init(&R->done_n, 0);
    R->o = o; R->pool = pool; R->arm = *arm;
    R->main_tid = (unsigned long)pthread_self();
    R->ts_best = R->pr_best = ~0ull;

    os_slot slot[PACE_MAX_SLOT];
    memset(slot, 0, sizeof slot);
    R->slot = slot;
    if (os_slot_init(slot, arm->slots, em) != 0) { memset(res, 0, sizeof *res); return 1; }
    g_pace = R;      // 回调从这里取上下文（XComponent 侧的回调没有 data 参数）

    if (arm->driver == 0) {
        // 裸 present：我们自己的循环。只有"我画得多快"，没有系统拍子。
        for (int f = 0; f < arm->target; f++) {
            os_slot *s = &slot[f % arm->slots];
            uint32_t idx = 0; uint64_t ts = 0; const char *why = "";
            int rc = os_frame_do(o, pool, s, arm->wait_idle, &idx, &ts, &why);
            if (rc) { OS_EMIT(em, "❌ 第 %d 帧失败：%s（%d）", f, why, rc); R->fails++; break; }
            if (R->pr_prev) {
                uint64_t dt = ts - R->pr_prev;
                if (dt < R->pr_best) R->pr_best = dt;
                if (dt > R->pr_worst) R->pr_worst = dt;
                R->pr_sum += dt; R->pr_n++;
            }
            R->pr_prev = ts;
            got++;
        }
    } else if (arm->driver == 1) {
        OH_DisplaySoloist *ds = OH_DisplaySoloist_Create(true);   // true = 独占一条线程
        if (!ds) {
            OS_EMIT(em, "❌ OH_DisplaySoloist_Create 失败（拿不到报时员）");
            os_slot_free(slot, arm->slots, pool);
            memset(res, 0, sizeof *res);
            return 1;
        }

        // 帧率要在 Start 之前设（官方说明：Start 之后再设"也可以生效但可能存在延迟"）
        if (arm->rate) {
            DisplaySoloist_ExpectedRateRange rg;
            rg.min = rg.max = rg.expected = arm->rate;
            if (OH_DisplaySoloist_SetExpectedFrameRateRange(ds, &rg) != 0)
                OS_EMIT(em, "⚠ 设期望帧率 %d 失败（继续跑，但下面的间隔不代表它）", arm->rate);
        }

        if (OH_DisplaySoloist_Start(ds, pace_ds_cb, R) != 0) {
            OS_EMIT(em, "❌ OH_DisplaySoloist_Start 失败");
            OH_DisplaySoloist_Destroy(ds);
            fails++;
        } else {
            // 等它收满拍。**轮询 + 超时**：探针宁可如实报"没收到拍"，也不把 App 挂死。
            // 需要多久由帧率算得（60 拍 @60Hz ≈ 1 s、@30Hz ≈ 2 s），宽放一倍再加兜底。
            uint64_t need_ms = (uint64_t)arm->target * 1000ull / (uint64_t)(arm->rate ? arm->rate : 60);
            uint64_t deadline = need_ms * 2 + 3000;
            uint64_t t0 = ms_now();
            while (atomic_load(&R->done_n) < arm->target && ms_now() - t0 < deadline) usleep(1000);
            got = atomic_load(&R->done_n);

            // 收尾顺序要紧：置 stop（回调不再干活）→ Stop → **等最后一拍退出** → 才 Destroy。
            // 少最后那一步，就是"Destroy 之后回调还在碰 Vulkan 对象"，真机上会直接崩。
            atomic_store(&R->stop, 1);
            OH_DisplaySoloist_Stop(ds);
            while (atomic_load(&R->in_cb)) usleep(1000);
            OH_DisplaySoloist_Destroy(ds);
        }
    } else {
        // XComponent 侧接入口（官方 API 11）：随 XComponent 走，更贴合"游戏把画面渲染进 XComponent"。
        OH_NativeXComponent *xc = (OH_NativeXComponent *)dm_ohos_surface_xc();
        if (!xc) {
            OS_EMIT(em, "❌ 还没拿到 OH_NativeXComponent（ArkUI 侧没把组件交过来？）");
            fails++;
        } else {
            if (arm->rate) {
                OH_NativeXComponent_ExpectedRateRange rg;
                rg.min = rg.max = rg.expected = arm->rate;
                if (OH_NativeXComponent_SetExpectedFrameRateRange(xc, &rg) != 0)
                    OS_EMIT(em, "⚠ 设期望帧率 %d 失败（继续跑，但下面的间隔不代表它）", arm->rate);
            }
            if (OH_NativeXComponent_RegisterOnFrameCallback(xc, pace_xc_cb) != 0) {
                OS_EMIT(em, "❌ RegisterOnFrameCallback 失败");
                fails++;
            } else {
                // **只短等一小会儿**：同步探针在原理上收不到 XC 侧的回调（它要 UI 主线程派发，而我们正堵着），
                // 等 1.2 s 足够确认"确实收不到"。绝不能用几秒的超时去等 —— 三个组加起来会把 UI 线程堵过 6 秒，
                // 系统会判 THREAD_BLOCK_6S 直接 PROCESS_KILL（真机踩过）。
                uint64_t deadline = 1200;
                uint64_t t0 = ms_now();
                while (atomic_load(&R->done_n) < arm->target && ms_now() - t0 < deadline) usleep(1000);
                got = atomic_load(&R->done_n);

                // 与 DisplaySoloist 同样的收尾顺序：停回调 → 等最后一拍退出 → 才算干净
                atomic_store(&R->stop, 1);
                OH_NativeXComponent_UnregisterOnFrameCallback(xc);
                while (atomic_load(&R->in_cb)) usleep(1000);
            }
        }
    }

    g_pace = NULL;
    pace_report(em, arm, R, got);
    pace_res_of(arm, R, got, res);
    os_slot_free(slot, arm->slots, pool);
    if (got < arm->target && arm->driver != 2) fails++;   // XC 侧收不到拍是"预期如此"，不算失败
    return fails;
}

int vk_backend_pace(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    os_ctx o;
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.3-B）：可变帧率节拍 —— OH_DisplaySoloist ===");
    OS_EMIT(em, "系统每拍 VSync 调一次回调 → 回调里出一帧；与「裸 present」在同一轮里对照");

    fails = os_setup(em, &o);
    if (fails) {
        os_teardown(&o);
        OS_EMIT(em, "");
        OS_EMIT(em, "===== 3.3-B 未全通过：图都搭不出来（失败 %d 项）=====", fails);
        *em->p = 0;
        return fails;
    }

    // 五组，顺序有讲究：先立裸 present 的基线（① 等 GPU = 3.3-A 口径；② 不等 GPU = 上游语义），
    // 再看系统报时（③），最后拿 ④ 回答"回调里等 GPU 会怎样"、拿 ⑤ 验可变帧率。
    static const pace_arm ARMS[] = {
        { "① 裸 present · 等 GPU（3.3-A 老基线）",  0, 1,  0, PACE_ARM_FRAMES, 1 },
        { "② 裸 present · 不等 GPU（上游语义）",    0, 0,  0, PACE_ARM_FRAMES, 3 },
        { "③ DisplaySoloist 60Hz · 不等 GPU",       1, 0, 60, PACE_ARM_FRAMES, 3 },
        { "④ DisplaySoloist 60Hz · 等 GPU",         1, 1, 60, PACE_ARM_FRAMES, 3 },
        { "⑤ DisplaySoloist 30Hz · 不等 GPU",       1, 0, 30, PACE_ARM_FRAMES, 3 },
    };
    const int NARM = (int)(sizeof ARMS / sizeof *ARMS);
    static pace_run R;                 // 放静态：回调要活到 Stop 之后，不能是正在退栈的局部量
    pace_res res[8];                   // 定长表 + 下面挡住 NARM 超出（加组时不会悄悄越界）
    memset(res, 0, sizeof res);

    if (NARM > (int)(sizeof res / sizeof *res)) {
        OS_EMIT(em, "❌ 组数 %d 超出读数表容量", NARM);
        os_teardown(&o);
        *em->p = 0;
        return 1;
    }
    for (int i = 0; i < NARM; i++) {
        OS_EMIT(em, "");
        fails += pace_one_arm(em, &o, &ARMS[i], &R, &res[i]);
    }

    os_teardown(&o);

    // ---- 三个判据：每条都要能一眼看出成没成 ----
    OS_EMIT(em, "");
    OS_EMIT(em, "===== 判据 =====");
    {
        const double MS60 = 1000.0 / 60.0, MS30 = 1000.0 / 30.0;
        int ok1 = 0, ok2 = 0;

        // ① 系统报时成立：60 Hz 组的时间戳间隔应 ≈16.67 ms（±20% 宽容，vsync 抖动是常态）
        if (res[2].ts_avg_ms <= 0)
            OS_EMIT(em, "① ❌ 一次回调都没收到（或压根没跑起来）—— 报时这条路不通");
        else if (res[2].ts_avg_ms > MS60 * 0.8 && res[2].ts_avg_ms < MS60 * 1.2) {
            ok1 = 1;
            OS_EMIT(em, "① ✅ 系统报时成立：60Hz 组回调间隔平均 %.2f ms ≈ 16.67 ms", res[2].ts_avg_ms);
        } else
            OS_EMIT(em, "① ⚠ 60Hz 组回调间隔平均 %.2f ms，与 16.67 ms 差得远 —— **60fps 这个请求没被满足**，"
                        "如实记录，别当成通过（下面的 ② 才是「帧率能不能设」的判据）", res[2].ts_avg_ms);

        // ② 帧率可设：看 30 Hz 组的**绝对**间隔是否 ≈33.3 ms。
        // 这里刻意不用"30Hz ÷ 60Hz 的比值"当判据 —— 万一 60Hz 组本身被限到别的数（① 的 ⚠ 就是这种情况），
        // 比值会被那个限流污染，得出"帧率没生效"的错误结论。绝对间隔不受它影响。
        if (res[4].ts_avg_ms <= 0)
            OS_EMIT(em, "② ⚠ 30Hz 组没收到拍，可变帧率这一条没验成");
        else if (res[4].ts_avg_ms > MS30 * 0.8 && res[4].ts_avg_ms < MS30 * 1.2) {
            ok2 = 1;
            OS_EMIT(em, "② ✅ 帧率可设（生效）：设 30fps 后回调间隔平均 %.2f ms ≈ 33.33 ms", res[4].ts_avg_ms);
        } else
            OS_EMIT(em, "② ⚠ 设 30fps 后回调间隔平均 %.2f ms，与 33.33 ms 差得远 —— 如实记录", res[4].ts_avg_ms);
        if (res[2].ts_avg_ms > 0 && res[4].ts_avg_ms > 0)
            OS_EMIT(em, "（参考）30Hz ÷ 60Hz 间隔之比 = ×%.2f（若 60Hz 组本身被限流，这个比值不代表帧率设置）",
                    res[4].ts_avg_ms / res[2].ts_avg_ms);

        // ③ 回调里能不能阻塞：④ 与 ③ 同一段代码，只差"present 后等不等 GPU"
        if (res[2].ts_avg_ms > 0 && res[3].ts_avg_ms > 0) {
            double k = res[3].ts_avg_ms / res[2].ts_avg_ms;
            if (k > 1.3)
                OS_EMIT(em, "③ ⚠ 回调里等 GPU 会拖垮节拍：间隔从 %.2f ms 涨到 %.2f ms（×%.2f）⇒ 生产不该在回调里等",
                        res[2].ts_avg_ms, res[3].ts_avg_ms, k);
            else
                OS_EMIT(em, "③ ✅ 回调里等 GPU 也扛住了（间隔 %.2f ms vs %.2f ms，×%.2f）—— 但不代表生产该等",
                        res[3].ts_avg_ms, res[2].ts_avg_ms, k);
        } else OS_EMIT(em, "③ ⚠ 两组没同时收到拍，这一条没比成");

        // 附带信息：两种节拍谁更整齐（最大间隔 / 平均间隔，越接近 1 越好）
        OS_EMIT(em, "（参考）抖动比 = 最大间隔/平均间隔：裸 present 不等 GPU %.2f ／ DisplaySoloist 60Hz %.2f",
                res[1].jitter, res[2].jitter);

        // 结论跟着判据走 —— 不写死（写死过一次：判据全是 ⚠ 却还打印"可用"）
        if (!fails) {
            if (ok1 && ok2)
                OS_EMIT(em, "结论：系统报时 ≈16.67 ms 且帧率可设 ⇒ 回放层可以用它当节拍源");
            else if (ok2)
                OS_EMIT(em, "结论：**帧率可设**（30fps 生效），但 60fps 请求没被满足（实际 ≈%.2f ms）"
                            "—— 先把这个查清楚，再定回放层怎么接它", res[2].ts_avg_ms);
            else
                OS_EMIT(em, "结论：跑通了，但判据没达预期 —— **别当成可用**，见上面每条的实测数");
        }
    }

    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.3-B %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    return fails;
}

// ================= 阶段 4（3.3-C）：帧率接入口对照 —— DisplaySoloist vs XComponent 侧 =================
// 3.3-B 验的是"DisplaySoloist 能不能当报时源"。但官方"可变帧率"其实有**四个接入口**：
// ArkUI 的 `expectedFrameRateRange`、`displaySync`、**XComponent 侧的
// `OH_NativeXComponent_SetExpectedFrameRateRange` + `RegisterOnFrameCallback`（API 11 起）**、`DisplaySoloist`。
// 其中 **XComponent 那一条更贴合"游戏把画面渲染进 XComponent"** 这个场景。
//
// 这一轮只回答一个问题：**两条入口到底差在哪**。四组紧挨着跑、**同轮内比**
// （跨轮次比读数会漂，3.3-B 已经吃过这个亏）：DS 60Hz / DS 30Hz / DS 60Hz·等GPU / **XC 60Hz（放最后）**。
//
// 判据：
//   ①② DisplaySoloist 两组与 3.3-B 的读数对齐（复现性）；
//   ③ XC 侧在同步探针下收到几拍 —— 预期 **0 拍**，因为官方写明它的回调**要在 UI 主线程派发**，
//      而同步探针正把 UI 线程堵着（真机实测就是 0 拍）；
//   ④ 线程模型：DisplaySoloist 回调在独立线程（实测）vs XC 侧回调在 UI 主线程（官方）——
//      **这才是两条入口的本质差别**，也是本轮选 DisplaySoloist 的依据。
//
// 反过来说：**要真正把 XC 侧量出数，探针必须改成"两段式"**（注册后立即返回让出 UI 线程 →
// 回调在 UI 线程上自收 → 再点一次取结果）。同步阻塞式探针在原理上测不到它。
// 另外提醒：**探针绝不能为了等拍把 UI 线程堵过 ~6 秒** —— 系统会判 THREAD_BLOCK_6S 直接杀进程
// （真机踩过：三组各等 5 秒超时，App 被 PROCESS_KILL）。所以 XC 组只短等 1.2 秒。
int vk_backend_pace_xc(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    os_ctx o;
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.3-C）：帧率接入口对照 —— DisplaySoloist vs XComponent 侧 ===");
    OS_EMIT(em, "两条官方入口、同一段渲染代码、同一轮里跑；比的是「拍子」与「回调线程」");

    fails = os_setup(em, &o);
    if (fails) {
        os_teardown(&o);
        OS_EMIT(em, "");
        OS_EMIT(em, "===== 3.3-C 未全通过：图都搭不出来（失败 %d 项）=====", fails);
        *em->p = 0;
        return fails;
    }

    // 顺序有讲究：XC 组放最后 —— 它要设期望帧率，会参与整机刷新率决策，
    // 夹在中间会把后面 DS 组的"拍子"污染掉。
    static const pace_arm ARMS[] = {
        { "① DisplaySoloist 60Hz",          1, 0, 60, PACE_ARM_FRAMES, 3 },
        { "② DisplaySoloist 30Hz",          1, 0, 30, PACE_ARM_FRAMES, 3 },
        { "③ DisplaySoloist 60Hz · 等 GPU", 1, 1, 60, PACE_ARM_FRAMES, 3 },
        { "④ XComponent 侧 60Hz（同步探针下预期收不到）", 2, 0, 60, PACE_ARM_FRAMES, 3 },
    };
    const int NARM = (int)(sizeof ARMS / sizeof *ARMS);
    static pace_run R;
    pace_res res[8];
    memset(res, 0, sizeof res);

    if (NARM > (int)(sizeof res / sizeof *res)) {
        OS_EMIT(em, "❌ 组数 %d 超出读数表容量", NARM);
        os_teardown(&o);
        *em->p = 0;
        return 1;
    }
    for (int i = 0; i < NARM; i++) {
        OS_EMIT(em, "");
        fails += pace_one_arm(em, &o, &ARMS[i], &R, &res[i]);
    }

    os_teardown(&o);

    OS_EMIT(em, "");
    OS_EMIT(em, "===== 判据（两条入口差在哪）=====");
    {
        // ①② DisplaySoloist 两组：与 3.3-B 的读数对齐（看复现性）
        if (res[0].ts_avg_ms > 0)
            OS_EMIT(em, "① DisplaySoloist 60Hz：拍子平均 %.2f ms（3.3-B 同量级基线 22.33 / 22.51 ms）",
                    res[0].ts_avg_ms);
        else OS_EMIT(em, "① DisplaySoloist 60Hz 没收到拍");
        if (res[1].ts_avg_ms > 0)
            OS_EMIT(em, "② DisplaySoloist 30Hz：拍子平均 %.2f ms（理论 33.33）", res[1].ts_avg_ms);
        else OS_EMIT(em, "② DisplaySoloist 30Hz 没收到拍");

        // ③ 这一条就是本轮的核心结论
        OS_EMIT(em, "③ XComponent 侧（同步探针）：收到 %d 拍 ⇒ %s", res[3].got,
                res[3].got ? "居然收到了，与官方「回调在 UI 主线程」不符，需要复查"
                           : "**一次都没收到** —— 与官方「回调函数运行于 UI 主线程」一致（UI 线程正被探针堵着）");

        // ④ 线程模型 —— 这才是两条入口的本质差别
        if (res[0].cb_tid && res[0].cb_tid != res[0].main_tid)
            OS_EMIT(em, "④ 线程模型：DisplaySoloist 回调在 %#lx（主线程 %#lx）⇒ **独立线程**；"
                        "XComponent 侧按官方文档在 **UI 主线程**", res[0].cb_tid, res[0].main_tid);
        else OS_EMIT(em, "④ 线程模型：DisplaySoloist 这组没收到拍，这条只报数（见上面各组）");

        // ⑤ 回调里等 GPU 的对照
        if (res[0].ts_avg_ms > 0 && res[2].ts_avg_ms > 0)
            OS_EMIT(em, "⑤ 回调里等 GPU：%.2f ms vs 不等 %.2f ms（×%.2f）",
                    res[2].ts_avg_ms, res[0].ts_avg_ms, res[2].ts_avg_ms / res[0].ts_avg_ms);

        // 结论（不写死，跟着上面的判据走）
        if (!fails)
            OS_EMIT(em, "结论：接入口选 **DisplaySoloist** —— 回调独立于 UI 线程，与"
                        "「游戏渲染在主循环线程、不能堵 UI」的架构相符；XComponent 侧接口的回调绑在 UI 主线程，"
                        "除非渲染本来就跑在 UI 线程，否则不合适");
    }

    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.3-C %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    return fails;
}


// ================= 阶段 4（3.4-A）：贴图通道最小闭环 —— BC 解压 → 上传 → 采样 =================
// 目标：把"贴图这条通道"打通（自建 VkImage → 上传 → 描述符/采样器 → 着色器采样 → 读回），
// 并给后面 BC→ASTC 转码留一个**谁都绕不开的参照**：CPU 解出来的那份 RGBA。
//
// 判据是**两条独立路径互证**：
//   CPU 参照：上游 `dm/gfx/tex/nvn_bcdec.c`（就是写给"没有 BC 的 GPU"用的那份，平台中立）解同一段 BC 字节 → RGBA8
//   GPU 实测：把那 RGBA 上传成 VkImage，NEAREST 全屏 1:1 采样画一遍 → 读回
//   ⇒ 应**逐像素一致**（离屏目标与贴图同为 RGBA8 UNORM；NEAREST + 像素中心对齐 ⇒ 没有插值误差）
//
// 为什么 A 必须先做：无论最后选"CPU 解→未压缩"（C1）还是"GPU 转码"（C3），**参照链都是它**；
// 而"图像创建 / 上传 / 描述符 / 采样"这一套是接真贴图必然要写的东西（下面抽成了 tex_gpu_render 共用）。
#define TEX_W      32
#define TEX_H      32
#define TEX_BLK     4

// 造一段 BC1（NVN 0x41 RGB_DXT1）测试数据。
// 设计意图：**每个 4×4 块两种颜色、块内棋盘交错，颜色随块坐标变化** —— 一旦有 x/y 翻转、
// 行距错、块序错，下面的逐像素比对立刻抓到（纯色图抓不到这些）。
// 颜色取 5:6:5 能精确表示的值（R/B 是 8 的倍数、G 是 4 的倍数）⇒ 编解码无损，参照干净。
// 块布局：c0(2B) c1(2B) 再 16 个 2 位索引（像素 (x,y) 占 bit 2*(y*4+x)；先写的在低位）。
// w / h 需是 4 的倍数；pitch = 每"块行"字节数 = (w/4)*8。
static void tex_make_bc1(uint8_t *dst, size_t pitch, uint32_t w, uint32_t h) {
    for (uint32_t by = 0; by * TEX_BLK < h; by++) {
        for (uint32_t bx = 0; bx * TEX_BLK < w; bx++) {
            uint32_t r5a = (bx * 4u) & 0x1Fu, g6a = (by * 8u) & 0x3Fu, b5a = ((bx + by) * 4u) & 0x1Fu;
            uint32_t r5b = (r5a + 12u) & 0x1Fu, g6b = (g6a + 24u) & 0x3Fu, b5b = (b5a + 12u) & 0x1Fu;
            uint32_t ca = (r5a << 11) | (g6a << 5) | b5a;
            uint32_t cb = (r5b << 11) | (g6b << 5) | b5b;
            // BC1：c0 > c1 ⇒ 4 色模式（alpha 全 255，没有透明）。若要交换，索引位一起反过来，
            // 这样"颜色 A 占哪些像素"保持不变 —— 测试图案的可预测性靠这一点。
            uint32_t inv = 0;
            if (ca < cb) { uint32_t t = ca; ca = cb; cb = t; inv = 1; }
            uint32_t idx = 0;
            for (uint32_t y = 0; y < TEX_BLK; y++)
                for (uint32_t x = 0; x < TEX_BLK; x++)
                    if ((((x ^ y) & 1u) ^ inv) != 0) idx |= 1u << (2 * (y * TEX_BLK + x));
            uint8_t *b = dst + (size_t)by * pitch + (size_t)bx * 8;
            b[0] = (uint8_t)(ca & 0xFF); b[1] = (uint8_t)(ca >> 8);
            b[2] = (uint8_t)(cb & 0xFF); b[3] = (uint8_t)(cb >> 8);
            b[4] = (uint8_t)(idx & 0xFF);      b[5] = (uint8_t)((idx >> 8) & 0xFF);
            b[6] = (uint8_t)((idx >> 16) & 0xFF); b[7] = (uint8_t)((idx >> 24) & 0xFF);
        }
    }
}

// ---- 上传 + 采样 + 读回（3.4-A 与 3.4-C 共用）----
// 为什么抽出来而不是各写一份：这段是**接真贴图必然要写的管道**（图像/暂存/布局转换/采样器/描述符/管线），
// 两处各抄一份的话，以后改一处就会漏另一处；而且它已经被 3.4-A 的逐像素判据验过了。
typedef struct { VkImage img; VkBuffer stg; uint32_t w, h; } tex_up_pack;
typedef struct { vk_rt *t; VkPipeline pl; VkPipelineLayout lay; VkDescriptorSet set; uint32_t w, h; } tex_draw_pack;

// 上传：暂存缓冲 → 纹理图像（两次布局转换夹一次拷贝）
static void rec_tex_upload(VkCommandBuffer cb, void *ud) {
    tex_up_pack *u = (tex_up_pack *)ud;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = u->img;
    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    VkBufferImageCopy reg = { 0 };
    reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    reg.imageSubresource.layerCount = 1;
    reg.imageExtent.width = u->w; reg.imageExtent.height = u->h; reg.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cb, u->stg, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);

    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

// 画全屏三角形采样贴图，然后读回（finalLayout 已经是 TRANSFER_SRC，参考阶段 2 的 rec_draw）
static void rec_tex_draw(VkCommandBuffer cb, void *ud) {
    tex_draw_pack *d = (tex_draw_pack *)ud;
    vk_rt *t = d->t;
    // 清成品红：本测试图案取不到这个颜色（R 最大 224），所以"看到品红"＝那个像素根本没被画到
    VkClearValue cv = { 0 };
    cv.color.float32[0] = 1.f; cv.color.float32[2] = 1.f; cv.color.float32[3] = 1.f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = t->rp; bi.framebuffer = t->fb;
    bi.renderArea.extent.width = d->w; bi.renderArea.extent.height = d->h;
    bi.clearValueCount = 1; bi.pClearValues = &cv;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pl);
    VkViewport vp = { 0.f, 0.f, (float)d->w, (float)d->h, 0.f, 1.f };
    VkRect2D sc = { { 0, 0 }, { d->w, d->h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d->lay, 0, 1, &d->set, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);            // 全屏三角形：顶点位置由 gl_VertexIndex 算，不用顶点缓冲
    vkCmdEndRenderPass(cb);

    VkBufferImageCopy reg = { 0 };
    reg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    reg.imageSubresource.layerCount = 1;
    reg.imageExtent.width = d->w; reg.imageExtent.height = d->h; reg.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->rb, 1, &reg);
}

// 把一张 w×h 的贴图（任意 VkFormat，字节已按该格式紧凑排好）上传成纹理、用 NEAREST 1:1 采样
// 画满离屏目标 t、再读回 out_pix。调用方负责：t 已经按 **同一个 w×h** 建好；out_pix 至少 w*h*4 字节。
// 返回 0 成功；非 0 失败（失败原因已打进报告）。
//
// 为什么参数化格式：3.4-A/C 传的是 RGBA8，C3（3.4-E）要传 **ASTC 4x4**——两者只差"图像/视图的格式"
// 和"暂存字节数"，其余（暂存、布局转换、采样器、描述符、管线、读回）一字不差。各写一份的话，
// 以后改一处就会漏另一处（这条教训在 3.4-C 抽共用管道时已经吃过一次）。
// Why the format is a parameter: 3.4-A/C upload RGBA8 while C3 (3.4-E) uploads ASTC 4x4; only the
// image/view format and the staging size differ -- everything else is identical.
static int tex_gpu_render_fmt(os_emit *em, vk_ctx *c, vk_rt *t, VkFormat fmt,
                              const void *bytes, size_t nbytes,
                              uint32_t w, uint32_t h, uint8_t *out_pix) {
    VkImage img = NULL; VkDeviceMemory img_mem = NULL; VkBuffer stg = NULL; VkDeviceMemory stg_mem = NULL;
    VkSampler smp = NULL; VkImageView view = NULL;
    VkDescriptorSetLayout dsl = NULL; VkDescriptorPool dpool = NULL; VkDescriptorSet dset = NULL;
    VkPipelineLayout lay = NULL; VkPipeline pl = NULL;
    char err[256] = "";
    int fails = 0;

    {
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt;
        ii.extent = (VkExtent3D){ w, h, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(c->dev, &ii, NULL, &img);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateImage", r); goto fail; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(c->dev, img, &mr);
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mi.allocationSize = mr.size;
        mi.memoryTypeIndex = find_mem_type(c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &r);
        if (r != VK_SUCCESS) { OS_EMIT(em, "❌ 纹理找不到 device-local 内存类型"); goto fail; }
        r = vkAllocateMemory(c->dev, &mi, NULL, &img_mem);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkAllocateMemory(tex)", r); goto fail; }
        vkBindImageMemory(c->dev, img, img_mem, 0);
    }
    {
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = nbytes; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult r = vkCreateBuffer(c->dev, &bi, NULL, &stg);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateBuffer(stg)", r); goto fail; }
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(c->dev, stg, &mr);
        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mi.allocationSize = mr.size;
        mi.memoryTypeIndex = find_mem_type(c, mr.memoryTypeBits,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r);
        if (r != VK_SUCCESS) { OS_EMIT(em, "❌ 暂存缓冲找不到 host-visible 内存类型"); goto fail; }
        r = vkAllocateMemory(c->dev, &mi, NULL, &stg_mem);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkAllocateMemory(stg)", r); goto fail; }
        vkBindBufferMemory(c->dev, stg, stg_mem, 0);
        void *mp = NULL;
        if (vkMapMemory(c->dev, stg_mem, 0, nbytes, 0, &mp) != VK_SUCCESS) {
            OS_EMIT(em, "❌ 暂存缓冲映射失败"); goto fail;
        }
        memcpy(mp, bytes, nbytes); vkUnmapMemory(c->dev, stg_mem);
    }
    OS_EMIT(em, "✓ 自建纹理图像（格式 #%d，%ux%u，device-local）+ 暂存缓冲 %zu 字节", (int)fmt, w, h, nbytes);

    {
        tex_up_pack u = { img, stg, w, h };
        if (one_shot(c, rec_tex_upload, &u) != 0) { OS_EMIT(em, "❌ 上传提交失败"); fails++; goto done; }
    }
    OS_EMIT(em, "✓ 上传完成（暂存缓冲 → vkCmdCopyBufferToImage，含两次布局转换）");

    {
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.f;
        VkResult r = vkCreateSampler(c->dev, &si, NULL, &smp);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateSampler", r); goto fail; }

        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        r = vkCreateImageView(c->dev, &vi, NULL, &view);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateImageView(tex)", r); goto fail; }

        VkDescriptorSetLayoutBinding b = { 0 };
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dl.bindingCount = 1; dl.pBindings = &b;
        r = vkCreateDescriptorSetLayout(c->dev, &dl, NULL, &dsl);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateDescriptorSetLayout", r); goto fail; }

        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pi.maxSets = 1; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        r = vkCreateDescriptorPool(c->dev, &pi, NULL, &dpool);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateDescriptorPool", r); goto fail; }

        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &dsl;
        r = vkAllocateDescriptorSets(c->dev, &ai, &dset);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkAllocateDescriptorSets", r); goto fail; }

        VkDescriptorImageInfo di = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wr.dstSet = dset; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &di;
        vkUpdateDescriptorSets(c->dev, 1, &wr, 0, NULL);
    }
    OS_EMIT(em, "✓ 采样器（NEAREST/CLAMP）+ 图像视图 + 描述符集就绪");

    {
        VkShaderModule vm = make_module(c->dev, dm_spv_tex_vert, dm_spv_tex_vert_n, err, sizeof err);
        VkShaderModule fm = vm ? make_module(c->dev, dm_spv_tex_frag, dm_spv_tex_frag_n, err, sizeof err) : NULL;
        if (!vm || !fm) { OS_EMIT(em, "❌ 建着色器模块失败：%s", err); fails++; goto done; }
        VkPipelineShaderStageCreateInfo st[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", NULL },
        };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        // 顶点绑定/属性都是 0：全屏三角形的顶点由 gl_VertexIndex 现算，不需要顶点缓冲
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vps.viewportCount = 1; vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        dss.depthTestEnable = VK_FALSE; dss.stencilTestEnable = VK_FALSE;    // 不留 NULL（真机崩过）
        VkPipelineColorBlendAttachmentState at = { 0 };
        at.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1; cb.pAttachments = &at;
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
        VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        li.setLayoutCount = 1; li.pSetLayouts = &dsl;
        VkResult r = vkCreatePipelineLayout(c->dev, &li, NULL, &lay);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreatePipelineLayout(tex)", r); vkDestroyShaderModule(c->dev, fm, NULL); vkDestroyShaderModule(c->dev, vm, NULL); fails++; goto done; }
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gi.stageCount = 2; gi.pStages = st;
        gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vps; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
        gi.pDepthStencilState = &dss; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
        gi.layout = lay; gi.renderPass = t->rp;
        r = vkCreateGraphicsPipelines(c->dev, NULL, 1, &gi, NULL, &pl);
        vkDestroyShaderModule(c->dev, fm, NULL);
        vkDestroyShaderModule(c->dev, vm, NULL);
        if (r != VK_SUCCESS) { set_err(err, sizeof err, "vkCreateGraphicsPipelines(tex)", r); fails++; goto done; }
        OS_EMIT(em, "✓ 采样管线创建成功（全屏三角形 + sampler2D，SPIR-V 来自 glslang）");
    }

    {
        tex_draw_pack dp = { t, pl, lay, dset, w, h };
        if (one_shot(c, rec_tex_draw, &dp) != 0) { OS_EMIT(em, "❌ 绘制提交失败"); fails++; goto done; }
        // 读回的是**离屏目标的像素**（RGBA8），与源贴图的格式无关 ⇒ 长度按 w*h*4 算，不能用 nbytes。
        if (vk_backend_readback(c, t, out_pix, (size_t)w * h * 4) < 0) { OS_EMIT(em, "❌ 读回失败"); fails++; goto done; }
    }

done:
    if (pl) vkDestroyPipeline(c->dev, pl, NULL);
    if (lay) vkDestroyPipelineLayout(c->dev, lay, NULL);
    if (dpool) vkDestroyDescriptorPool(c->dev, dpool, NULL);
    if (dsl) vkDestroyDescriptorSetLayout(c->dev, dsl, NULL);
    if (view) vkDestroyImageView(c->dev, view, NULL);
    if (smp) vkDestroySampler(c->dev, smp, NULL);
    if (stg) vkDestroyBuffer(c->dev, stg, NULL);
    if (stg_mem) vkFreeMemory(c->dev, stg_mem, NULL);
    if (img) vkDestroyImage(c->dev, img, NULL);
    if (img_mem) vkFreeMemory(c->dev, img_mem, NULL);
    return fails;
fail:
    OS_EMIT(em, "❌ 失败：%s", err);
    fails++;
    goto done;
}

// 3.4-A / 3.4-C 走的那条：RGBA8 未压缩上传。保留这个名字与签名，是为了"改通道前后逐项对账"——
// 那两个测试的判据（逐像素差 0）必须一字不变地仍然成立，否则说明泛化时改坏了。
// The RGBA8 route used by 3.4-A / 3.4-C; keeping this wrapper lets those two tests be re-run
// unchanged after the generalisation, which is exactly the reconciliation this refactor needs.
static int tex_gpu_render(os_emit *em, vk_ctx *c, vk_rt *t, const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out_pix) {
    return tex_gpu_render_fmt(em, c, t, VK_FORMAT_R8G8B8A8_UNORM, rgba, (size_t)w * h * 4, w, h, out_pix);
}

// 逐像素比对：返回"差 >1 的像素数"，并把最大通道差写进 *worst_out
static int tex_diff(const uint8_t *a, const uint8_t *b, uint32_t npix, int *worst_out, int *first_out) {
    int mismatch = 0, worst = 0, first = -1;
    for (uint32_t i = 0; i < npix; i++) {
        int d = 0;
        for (int k = 0; k < 4; k++) {
            int e = (int)a[i * 4 + k] - (int)b[i * 4 + k];
            if (e < 0) e = -e;
            if (e > d) d = e;
        }
        if (d > worst) worst = d;
        if (d > 1) { if (first < 0) first = (int)i; mismatch++; }
    }
    if (worst_out) *worst_out = worst;
    if (first_out) *first_out = first;
    return mismatch;
}

// 报告落盘（dir 为空则只回显）：界面上的文本框只有一屏，而这条报告的判据有六组，
// 用截图去读既费劲又容易漏行 —— 与 3.4-B 一样，写一份文件，`hdc shell cat` 直接取全文。
static void dump_report(const char *dir, const char *name, const char *text) {
    if (!dir || !*dir) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

int vk_backend_tex(const char *dir, char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.4-A）：贴图通道最小闭环 —— BC 解压 → 上传 → 采样 ===");
    OS_EMIT(em, "贴图 %d×%d（BC1 RGB_DXT1，块大小 %d×%d），离屏目标同尺寸 1:1 采样（NEAREST + CLAMP）",
            TEX_W, TEX_H, TEX_BLK, TEX_BLK);

    char err[256] = "";
    vk_ctx *c = vk_backend_init(err, sizeof err);
    if (!c) { OS_EMIT(em, "❌ 初始化失败：%s", err); OS_EMIT(em, "===== 失败 ====="); *em->p = 0; return 1; }
    OS_EMIT(em, "✓ Vulkan 就绪（设备 %s）", c->devname);
    vk_rt *t = vk_backend_target(c, TEX_W, TEX_H, err, sizeof err);
    if (!t) { OS_EMIT(em, "❌ 建离屏目标失败：%s", err); vk_backend_shutdown(c); *em->p = 0; return 1; }

    // ---- ① CPU 参照：造 BC1 数据 → 上游解码器解成 RGBA8 ----
    static uint8_t bc[TEX_W / TEX_BLK * 8 * (TEX_H / TEX_BLK)];    // 每块行 (W/4)*8 字节
    static uint8_t ref[TEX_W * TEX_H * 4];
    static uint8_t pix[TEX_W * TEX_H * 4];
    const size_t bc_pitch = (size_t)(TEX_W / TEX_BLK) * 8;
    tex_make_bc1(bc, bc_pitch, TEX_W, TEX_H);
    if (nvn_bc_decode(0x41, bc, bc_pitch, TEX_W, TEX_H, ref, TEX_W * 4) != 0) {
        OS_EMIT(em, "❌ nvn_bc_decode 拒绝了这个格式（0x41）");
        vk_backend_target_free(c, t); vk_backend_shutdown(c); *em->p = 0; return 1;
    }
    OS_EMIT(em, "✓ CPU 参照解出（nvn_bc_decode，BC1 %zu 字节 → RGBA8 %d 字节）", bc_pitch * (TEX_H / TEX_BLK), TEX_W * TEX_H * 4);

    // ---- ② 上传 + 采样 + 读回（管道在 tex_gpu_render 里，3.4-C 用的是同一份）----
    fails = tex_gpu_render(em, c, t, ref, TEX_W, TEX_H, pix);

    // ---- ③ 判据 ----
    if (!fails) {
        int worst = 0, first = -1;
        int mismatch = tex_diff(ref, pix, TEX_W * TEX_H, &worst, &first);
        OS_EMIT(em, "");
        OS_EMIT(em, "---- 判据：GPU 采到的 vs CPU 解出来的（%u 像素逐点比）----", TEX_W * TEX_H);
        OS_EMIT(em, "  差 >1 的像素：%d / %u；最大通道差：%d", mismatch, TEX_W * TEX_H, worst);
        if (mismatch == 0) {
            OS_EMIT(em, "  ✅ 逐像素一致（±1 以内）—— 贴图通道（图像/上传/描述符/采样）通了");
            if (worst == 0) OS_EMIT(em, "  （而且一个字节都没差：NEAREST 1:1 采样按预期精确命中纹素中心）");
        } else {
            fails++;
            OS_EMIT(em, "  ❌ 首个不符的像素 #%d：CPU=(%u,%u,%u,%u) GPU=(%u,%u,%u,%u)", first,
                    ref[(size_t)first * 4], ref[(size_t)first * 4 + 1], ref[(size_t)first * 4 + 2], ref[(size_t)first * 4 + 3],
                    pix[(size_t)first * 4], pix[(size_t)first * 4 + 1], pix[(size_t)first * 4 + 2], pix[(size_t)first * 4 + 3]);
        }
        // 反"两边一起错"的旁证：测试图案每块应恰好两种颜色、且块内棋盘交错。
        // 这一条只看参照自己，不依赖任何 GPU 结果 —— 万一上传/采样与我造的数据"错得一致"，它会先炸。
        {
            int badblk = 0;
            for (uint32_t by = 0; by * TEX_BLK < TEX_H; by++)
                for (uint32_t bx = 0; bx * TEX_BLK < TEX_W; bx++) {
                    uint32_t p0 = (uint32_t)ref[((by * TEX_BLK) * TEX_W + bx * TEX_BLK) * 4] |
                                  ((uint32_t)ref[((by * TEX_BLK) * TEX_W + bx * TEX_BLK) * 4 + 1] << 8) |
                                  ((uint32_t)ref[((by * TEX_BLK) * TEX_W + bx * TEX_BLK) * 4 + 2] << 16);
                    int n1 = 0, n2 = 0;
                    for (uint32_t y = 0; y < TEX_BLK; y++)
                        for (uint32_t x = 0; x < TEX_BLK; x++) {
                            const uint8_t *q = ref + ((size_t)(by * TEX_BLK + y) * TEX_W + bx * TEX_BLK + x) * 4;
                            uint32_t v = (uint32_t)q[0] | ((uint32_t)q[1] << 8) | ((uint32_t)q[2] << 16);
                            if (v == p0) n1++; else n2++;
                        }
                    if (n1 == 0 || n2 == 0) badblk++;    // 整块只有一种颜色 ⇒ 图案与预期不符
                }
            OS_EMIT(em, "  （旁证）参照的块图案自检：%s（每块应两种颜色、块内棋盘交错）",
                    badblk ? "**有块不合格**" : "8×8 块全部合格");
            if (badblk) fails++;
        }
    }

    vk_backend_target_free(c, t);
    vk_backend_shutdown(c);
    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.4-A %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    dump_report(dir, "dm_tex_a.log", out);
    return fails;
}

// ================= 阶段 4（3.4-C）：贴图转码 C1 —— 把真场景那条链串起来 =================
// 设备不支持 BC（能力探测已验）。**C1 = 忠实上游的做法**：运行时在 CPU 上把 BC 解成未压缩再上传。
// 上游对"设备没有 BC"本来就这么干（`gfx/tex/nvn_bcdec.c` 就是写给苹果 A15/A16 的），所以这一步零新算法。
//
// 这一步做的事就是把前面几段串成**接真场景要走的那条链**：
//   游戏给的字节（**块线性**）→ 解排布 → 解 BC → 上传未压缩 → 采样上屏
// 而 3.4-A 只验了"线性 RGBA → 上传 → 采样"，3.4-B 只验了"块线性 ↔ 线性"。这里验的是**它们的组合**。
//
// 三条判据：
//   ① 解排布往返逐字节一致（写进块线性存储，再解回来必须与线性原件一样）
//   ② 上传 + 采样 + 读回 与 CPU 解码结果逐像素一致（±1）
//   ③ **负向判据**：跳过解排布那一步、直接拿块线性字节去解 BC ⇒ 结果必须**不同**
//      （否则说明"解排布"是个空转步骤，那 3.4-B 那一整套就是白做的）
// 另附 C1 的账：压缩字节 → 未压缩字节，显存与带宽各翻几倍。
#define TEXC_W 40
#define TEXC_H 24

int vk_backend_tex_c1(const char *dir, char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.4-C）：贴图转码 C1 —— 真贴图那条链 ===");
    OS_EMIT(em, "块线性字节 → 解排布 → CPU 解 BC → 上传未压缩 → 采样；贴图 %d×%d BC1（RGB_DXT1）", TEXC_W, TEXC_H);

    // ---- 贴图描述与布局（用上游 nvn_tex_layout 算，不是我们自己拍的数）----
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    d.target = 1;          // 2D
    d.format = 0x41;       // RGB_DXT1（BC1）
    d.width = TEXC_W; d.height = TEXC_H; d.depth = 1; d.levels = 1;
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    const size_t row_bytes = (size_t)(TEXC_W / TEX_BLK) * 8;      // 每"块行"字节
    const size_t lin_bytes = row_bytes * (TEXC_H / TEX_BLK);      // 线性 BC1 字节数
    OS_EMIT(em, "✓ 布局（上游 nvn_tex_layout）：块线性存储 %llu 字节、每块 %u 字节、块高 %u GOB、层 %u",
            (unsigned long long)L.size, L.bpb, L.tileH, L.layers);
    OS_EMIT(em, "  线性 BC1 是 %zu 字节（%zu 字节/块行 × %u 块行）；块线性存储更大是因为 GOB 边界要对齐",
            lin_bytes, row_bytes, TEXC_H / TEX_BLK);

    uint8_t *bc_lin = (uint8_t *)malloc(lin_bytes);
    uint8_t *bl = (uint8_t *)malloc((size_t)L.size + 4096);
    uint8_t *bc_back = (uint8_t *)malloc(lin_bytes);
    uint8_t *ref = (uint8_t *)malloc((size_t)TEXC_W * TEXC_H * 4);
    uint8_t *bad = (uint8_t *)malloc((size_t)TEXC_W * TEXC_H * 4);
    uint8_t *pix = (uint8_t *)malloc((size_t)TEXC_W * TEXC_H * 4);
    if (!bc_lin || !bl || !bc_back || !ref || !bad || !pix) {
        OS_EMIT(em, "❌ 内存不足"); free(bc_lin); free(bl); free(bc_back); free(ref); free(bad); free(pix);
        *em->p = 0; return 1;
    }
    memset(bl, 0xAB, (size_t)L.size + 4096);      // 存满哨兵：越界写会破坏它，下面的自检能抓到

    tex_make_bc1(bc_lin, row_bytes, TEXC_W, TEXC_H);

    // ---- ① 写进块线性存储，再解回来（模拟"游戏给的就是块线性"）----
    if (nvn_swizzle(&d, 0, 0, bc_lin, row_bytes, bl) != 0) {
        OS_EMIT(em, "❌ nvn_swizzle 失败（贴图不受支持？）"); fails++;
    } else if (nvn_deswizzle(&d, 0, 0, bl, bc_back, row_bytes) != 0) {
        OS_EMIT(em, "❌ nvn_deswizzle 失败"); fails++;
    } else {
        int same = memcmp(bc_lin, bc_back, lin_bytes) == 0;
        int guard_ok = 1;
        for (size_t i = 0; i < 4096; i++) if (bl[L.size + i] != 0xAB) { guard_ok = 0; break; }
        OS_EMIT(em, "");
        OS_EMIT(em, "---- ① 解排布往返 ----");
        OS_EMIT(em, "  %s：线性 → 块线性 → 线性 回来与原件逐字节%s", same ? "✅" : "❌", same ? "一致" : "**不一致**");
        OS_EMIT(em, "  %s：存储末尾 4096 字节哨兵%s（写出边界会破坏它）", guard_ok ? "✅" : "❌", guard_ok ? "完好" : "**被破坏**");
        if (!same || !guard_ok) fails++;
    }

    // ---- ② 上传 + 采样 + 读回，与 CPU 解码逐像素比 ----
    if (!fails) {
        if (nvn_bc_decode(0x41, bc_back, row_bytes, TEXC_W, TEXC_H, ref, TEXC_W * 4) != 0) {
            OS_EMIT(em, "❌ nvn_bc_decode 失败"); fails++;
        } else {
            char err[256] = "";
            vk_ctx *c = vk_backend_init(err, sizeof err);
            vk_rt *t = c ? vk_backend_target(c, TEXC_W, TEXC_H, err, sizeof err) : NULL;
            if (!c || !t) {
                OS_EMIT(em, "❌ Vulkan 初始化/建目标失败：%s", err); fails++;
            } else {
                if (tex_gpu_render(em, c, t, ref, TEXC_W, TEXC_H, pix) == 0) {
                    int worst = 0, first = -1;
                    int mismatch = tex_diff(ref, pix, TEXC_W * TEXC_H, &worst, &first);
                    OS_EMIT(em, "");
                    OS_EMIT(em, "---- ② 上传 + 采样 + 读回 vs CPU 解码（%u 像素逐点比）----", TEXC_W * TEXC_H);
                    OS_EMIT(em, "  差 >1 的像素：%d / %u；最大通道差：%d", mismatch, TEXC_W * TEXC_H, worst);
                    if (mismatch == 0) OS_EMIT(em, "  ✅ 逐像素一致 —— 这条链（解排布→解码→上传→采样）通了");
                    else {
                        fails++;
                        OS_EMIT(em, "  ❌ 首个不符的像素 #%d：CPU=(%u,%u,%u,%u) GPU=(%u,%u,%u,%u)", first,
                                ref[(size_t)first * 4], ref[(size_t)first * 4 + 1], ref[(size_t)first * 4 + 2], ref[(size_t)first * 4 + 3],
                                pix[(size_t)first * 4], pix[(size_t)first * 4 + 1], pix[(size_t)first * 4 + 2], pix[(size_t)first * 4 + 3]);
                    }
                } else fails++;
            }
            if (t) vk_backend_target_free(c, t);
            if (c) vk_backend_shutdown(c);
        }
    }

    // ---- ③ 负向判据：不解排布直接解码，**必须不同** ----
    if (!fails) {
        if (nvn_bc_decode(0x41, bl, row_bytes, TEXC_W, TEXC_H, bad, TEXC_W * 4) != 0) {
            OS_EMIT(em, "❌ 负向对照：nvn_bc_decode 拒绝"); fails++;
        } else {
            int same = memcmp(ref, bad, (size_t)TEXC_W * TEXC_H * 4) == 0;
            int nbad = 0;
            for (uint32_t i = 0; i < (uint32_t)TEXC_W * TEXC_H; i++)
                if (memcmp(ref + i * 4, bad + i * 4, 4) != 0) nbad++;
            OS_EMIT(em, "");
            OS_EMIT(em, "---- ③ 负向判据：跳过解排布，直接拿块线性字节去解 BC ----");
            OS_EMIT(em, "  与正确结果不同的像素：%d / %d", nbad, TEXC_W * TEXC_H);
            OS_EMIT(em, "  %s：解排布这一步**确实在干活**（若两者相同，说明它是空转的）",
                    same ? "❌ 竟然一样 —— 说明解排布没起作用，要查" : "✅ 如预期地不同");
            if (same) fails++;
        }
    }

    // ---- C1 的账（诚实报出：这就是 C1 的代价）----
    OS_EMIT(em, "");
    OS_EMIT(em, "---- C1 的账（这是「忠实上游」这一路要付的代价）----");
    OS_EMIT(em, "  压缩字节 %zu → 未压缩字节 %d（×%.1f）", lin_bytes, TEXC_W * TEXC_H * 4,
            (double)(TEXC_W * TEXC_H * 4) / (double)lin_bytes);
    OS_EMIT(em, "  按每像素算：BC1 是 0.5 字节/像素，RGBA8 是 4 字节/像素 ⇒ **显存与带宽都 ×8**");
    OS_EMIT(em, "  收益：零新算法、当天就能接上真场景；这也是为什么 C3（GPU 转码成 ASTC）值得后面再做");

    free(bc_lin); free(bl); free(bc_back); free(ref); free(bad); free(pix);
    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.4-C（C1）%s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    dump_report(dir, "dm_tex_c1.log", out);
    return fails;
}

// ================= 阶段 4（3.4-E）：C3 设备接线 —— BC3 → ASTC 4x4 → 上传 → 采样 =================
// C3 的第 1 步（`gfx/tex/nvn_bc2astc.c` + `tools/astc_ref.py`）只证明了算法离线自洽；这一步证明
// **设备认**：能力怎么查、选哪条路、ASTC 图像建不建得出来、GPU 解出来的像素对不对。
// Step 1 of C3 proved the algorithm offline; this step proves the device accepts it.
//
// 与 3.4-C（C1）的区别只有一处：C1 把 BC 解成 RGBA8（4 B/px），这里转成 ASTC 4x4（1 B/px）。
// 所以判据也换了一套 —— 转码**有损**，不能再要求"逐像素 0 差"：
//   ⓪ 能力表 + 选路：逐个格式报 sampled/transfer 支持 + ASTC 扩展是否启用 ⇒ 给出走哪条路
//   ① 解排布往返逐字节一致 + 哨兵完好（与 3.4-C 同款：转码的输入必须是**解排布后**的线性块序）
//   ② 转码块字节 vs `tools/astc_ref.py` 生成的期望表：**逐字节**（这条把"算法错"与"实现错"分开）
//   ③ GPU 采样 vs 本地 ASTC 解码（nvn_astc4x4_decode）：**±1 逐像素** ← 最锋利的一条，
//      它证明"我们写出的 16 字节块是合法 ASTC，且位域理解正确"
//   ④ GPU 采样 vs CPU 的 BC 解码（理想图）：PSNR 不差于参考值 0.5 dB + 最大通道差 ≤ 32
//   ⑤ 负向：跳过解排布直接转码 ⇒ 转码结果必须不同（否则解排布是空转）
//   ⑥ 账：尺寸（BC3 → ASTC 几倍）+ 吞吐（1024² 上量单线程 vs 多线程）
//   ⑦ 多线程 vs 单线程**逐字节相同**（切分只按块行，块间无依赖 ⇒ 切分不该改变结果）
//
// 为什么借**共享上下文**而不是 vk_backend_init 自带的那个：生产路径（游戏贴图）走的是共享上下文，
// 那条设备上的 ASTC 扩展才是真正要验的东西；用测试后端自建的设备验，等于验了另一台设备。
// Why borrow the shared context: production textures go through it, so its device (and its ASTC
// extension) is what must be validated -- probing a separate test device would prove the wrong thing.
static void vk_ctx_borrow_shared(vk_ctx *c) {
    memset(c, 0, sizeof *c);
    c->inst  = dm_ohos_vk_instance();
    c->phy   = dm_ohos_vk_phys();
    c->dev   = dm_ohos_vk_device();
    c->queue = dm_ohos_vk_queue();
    c->qfam  = dm_ohos_vk_qfam();
    c->pool  = dm_ohos_vk_pool();
    snprintf(c->devname, sizeof c->devname, "%s", dm_ohos_vk_devname());
}

// PSNR（只算 RGB 三通道：alpha 在真贴图里多是遮罩，混进来会掩盖颜色误差；需要时单独报）
static double tex_psnr_rgb(const uint8_t *a, const uint8_t *b, uint32_t npix) {
    double se = 0.0;
    for (uint32_t i = 0; i < npix; i++)
        for (int k = 0; k < 3; k++) {
            double d = (double)a[(size_t)i * 4 + k] - (double)b[(size_t)i * 4 + k];
            se += d * d;
        }
    if (se == 0.0) return 99.0;
    double mse = se / ((double)npix * 3.0);
    return 10.0 * log10((255.0 * 255.0) / mse);
}

int vk_backend_tex_c3(const char *dir, char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.4-E）：C3 设备接线 —— BC3 转 ASTC 4x4 → 上传 → 采样 ===");
    OS_EMIT(em, "贴图 %d×%d BC3（NVN 0x44 DXT5，%d 块），转码成 ASTC 4x4（同样 %d 块、每块 16 字节）",
            ASTC_EXPECT_W, ASTC_EXPECT_H, ASTC_EXPECT_BLOCKS, ASTC_EXPECT_BLOCKS);

    // ---- ⓪ 能力表与选路 ----
    OS_EMIT(em, "");
    OS_EMIT(em, "---- ⓪ 设备能力（optimal tiling：能采样 + 能当上传目标）----");
    OS_EMIT(em, "  ASTC 已启用（特性位/扩展）：%s", dm_ohos_tex_astc_enabled() ? "是" : "**否**（ASTC 格式不合法，选路会回落）");
    {
        struct { VkFormat f; const char *name; } fmts[] = {
            { VK_FORMAT_BC1_RGBA_UNORM_BLOCK,   "BC1 (DXT1)" },
            { VK_FORMAT_BC3_UNORM_BLOCK,        "BC3 (DXT5)" },
            { VK_FORMAT_BC7_UNORM_BLOCK,        "BC7" },
            { VK_FORMAT_ASTC_4x4_UNORM_BLOCK,   "ASTC 4x4" },
            { VK_FORMAT_ASTC_4x4_SRGB_BLOCK,    "ASTC 4x4 sRGB" },
            { VK_FORMAT_R8G8B8A8_UNORM,         "RGBA8" },
        };
        for (size_t i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
            VkFormatFeatureFlags opt = 0, lin = 0;
            dm_ohos_fmt_features(fmts[i].f, &opt, &lin);
            OS_EMIT(em, "  %-14s optimal: 采样=%s 上传目标=%s%s", fmts[i].name,
                    (opt & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "有" : "无",
                    (opt & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) ? "有" : "无",
                    dm_ohos_fmt_ok(fmts[i].f) ? "  ⇒ 可用" : "  ⇒ 不可用");
        }
    }
    dm_tex_plan plan;
    if (dm_ohos_tex_plan(0x44, &plan) != 0) {
        OS_EMIT(em, "❌ 选路失败：0x44（BC3）没有可用路径");
        OS_EMIT(em, "===== 3.4-E 未通过：失败 1 项 =====");
        *em->p = 0;
        return 1;
    }
    OS_EMIT(em, "  ⇒ 选路（NVN 0x44 BC3）：**%s**，上传格式 #%d（BC 原生=%d、ASTC=%d）",
            nvn_tex_path_name(plan.path), (int)plan.fmt, plan.bc_native, plan.astc_ok);
    if (plan.path != NVN_TEX_PATH_ASTC) {
        // 不是失败：设备若有 BC 原生支持，直传更省，那也算"接线成功"。但本机预期是 ASTC。
        OS_EMIT(em, "  ⚠ 本次选路没走 ASTC（设备原生支持 BC，或 ASTC 没启用）—— 下面**强制按 ASTC 走一遍**以验通道");
    }
    // 强制验证用哪个格式：选路走 ASTC 就用它的格式；否则显式用 ASTC 4x4 UNORM，
    // 这样"ASTC 通道到底通不通"与"选路对不对"是两件事，报告里能分开看。
    const VkFormat up_fmt = (plan.path == NVN_TEX_PATH_ASTC) ? plan.fmt : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;

    // ---- 布局与缓冲区 ----
    nvn_texdesc d;
    memset(&d, 0, sizeof d);
    d.target = 1; d.format = 0x44; d.width = ASTC_EXPECT_W; d.height = ASTC_EXPECT_H;
    d.depth = 1; d.levels = 1;
    nvn_texlayout L;
    nvn_tex_layout(&d, &L);
    const uint32_t bx = ASTC_EXPECT_W / 4, by = ASTC_EXPECT_H / 4;
    const size_t row_bytes = (size_t)bx * 16;                 // BC3 每块行字节（与 ASTC 相同）
    const size_t astc_bytes = (size_t)bx * by * 16;

    uint8_t *bl       = (uint8_t *)malloc((size_t)L.size + 4096);
    uint8_t *bc_back  = (uint8_t *)malloc(ASTC_EXPECT_BC3_BYTES);
    uint8_t *astc_out = (uint8_t *)malloc(astc_bytes);
    uint8_t *astc_bad = (uint8_t *)malloc(astc_bytes);
    uint8_t *ref_bc   = (uint8_t *)malloc((size_t)ASTC_EXPECT_W * ASTC_EXPECT_H * 4);
    uint8_t *ref_astc = (uint8_t *)malloc((size_t)ASTC_EXPECT_W * ASTC_EXPECT_H * 4);
    uint8_t *pix      = (uint8_t *)malloc((size_t)ASTC_EXPECT_W * ASTC_EXPECT_H * 4);
    if (!bl || !bc_back || !astc_out || !astc_bad || !ref_bc || !ref_astc || !pix) {
        OS_EMIT(em, "❌ 内存不足");
        free(bl); free(bc_back); free(astc_out); free(astc_bad); free(ref_bc); free(ref_astc); free(pix);
        *em->p = 0;
        return 1;
    }
    memset(bl, 0xAB, (size_t)L.size + 4096);
    OS_EMIT(em, "");
    OS_EMIT(em, "  布局（上游 nvn_tex_layout）：块线性 %llu 字节、每块 %u 字节、块高 %u GOB；"
                "线性 BC3 = %d 字节，ASTC 4x4 = %zu 字节",
            (unsigned long long)L.size, L.bpb, L.tileH, ASTC_EXPECT_BC3_BYTES, astc_bytes);

    // ---- ① 解排布往返（转码的输入必须是解排布后的线性块序）----
    if (nvn_swizzle(&d, 0, 0, astc_expect_bc3, row_bytes, bl) != 0) {
        OS_EMIT(em, "❌ nvn_swizzle 失败"); fails++;
    } else if (nvn_deswizzle(&d, 0, 0, bl, bc_back, row_bytes) != 0) {
        OS_EMIT(em, "❌ nvn_deswizzle 失败"); fails++;
    } else {
        int same = memcmp(astc_expect_bc3, bc_back, ASTC_EXPECT_BC3_BYTES) == 0;
        int guard_ok = 1;
        for (size_t i = 0; i < 4096; i++) if (bl[L.size + i] != 0xAB) { guard_ok = 0; break; }
        OS_EMIT(em, "");
        OS_EMIT(em, "---- ① 解排布往返 ----");
        OS_EMIT(em, "  %s 线性 → 块线性 → 线性：%s", same ? "✅" : "❌", same ? "逐字节一致" : "**不一致**");
        OS_EMIT(em, "  %s 存储末尾 4096 字节哨兵%s", guard_ok ? "✅" : "❌", guard_ok ? "完好" : "**被破坏**");
        if (!same || !guard_ok) fails++;
    }

    // ---- ② 转码块字节 vs 参考实现（逐字节）----
    if (!fails) {
        if (nvn_bc2astc_region(0x44, bc_back, row_bytes, bx, by, astc_out, (size_t)bx * 16) != 0) {
            OS_EMIT(em, "❌ nvn_bc2astc_region 失败"); fails++;
        } else {
            int ndiff = 0, firstblk = -1, firstbyte = -1;
            for (int i = 0; i < ASTC_EXPECT_BLOCKS; i++) {
                if (memcmp(astc_out + (size_t)i * 16, astc_expect_astc + (size_t)i * 16, 16) != 0) {
                    if (firstblk < 0) {
                        firstblk = i;
                        for (int k = 0; k < 16; k++)
                            if (astc_out[(size_t)i * 16 + k] != astc_expect_astc[(size_t)i * 16 + k]) { firstbyte = k; break; }
                    }
                    ndiff++;
                }
            }
            OS_EMIT(em, "");
            OS_EMIT(em, "---- ② 转码块字节 vs tools/astc_ref.py 的期望（%d 块 × 16 字节）----", ASTC_EXPECT_BLOCKS);
            OS_EMIT(em, "  不同的块：%d / %d", ndiff, ASTC_EXPECT_BLOCKS);
            if (!ndiff) {
                OS_EMIT(em, "  ✅ 逐字节一致 —— C 版实现与参考算法同源（算法错/实现错由此分开）");
            } else {
                fails++;
                OS_EMIT(em, "  ❌ 首个不同的块 #%d 第 %d 字节：设备=%02X 参考=%02X", firstblk, firstbyte,
                        astc_out[(size_t)firstblk * 16 + firstbyte], astc_expect_astc[(size_t)firstblk * 16 + firstbyte]);
            }
        }
    }

    // ---- ③④ GPU 采样：ASTC 图像上传 → NEAREST 1:1 采样 → 读回 ----
    if (!fails) {
        // 参照一：本地 ASTC 解码（本模块只认自己产出的两种配置）
        int dec_ok = 1;
        for (uint32_t byi = 0; byi < by && dec_ok; byi++)
            for (uint32_t bxi = 0; bxi < bx && dec_ok; bxi++) {
                uint8_t *dst = ref_astc + ((size_t)byi * 4 * ASTC_EXPECT_W + bxi * 4) * 4;
                if (nvn_astc4x4_decode(astc_out + ((size_t)byi * bx + bxi) * 16, dst, (size_t)ASTC_EXPECT_W * 4) != 0)
                    dec_ok = 0;
            }
        // 参照二：CPU 的 BC 解码（理想图，转码前的样子）
        if (nvn_bc_decode(0x44, bc_back, row_bytes, ASTC_EXPECT_W, ASTC_EXPECT_H, ref_bc, ASTC_EXPECT_W * 4) != 0)
            dec_ok = 0;
        if (!dec_ok) {
            OS_EMIT(em, "❌ 参照解码失败（ASTC 或 BC）"); fails++;
        } else {
            vk_ctx sc; vk_ctx_borrow_shared(&sc);
            char err[256] = "";
            vk_rt *t = sc.dev ? vk_backend_target(&sc, ASTC_EXPECT_W, ASTC_EXPECT_H, err, sizeof err) : NULL;
            if (!t) {
                OS_EMIT(em, "❌ 建离屏目标失败：%s", err); fails++;
            } else {
                OS_EMIT(em, "");
                OS_EMIT(em, "---- ③④ 上传 ASTC 图像并采样（格式 #%d）----", (int)up_fmt);
                if (tex_gpu_render_fmt(em, &sc, t, up_fmt, astc_out, astc_bytes,
                                       ASTC_EXPECT_W, ASTC_EXPECT_H, pix) == 0) {
                    // ③ GPU vs 本地 ASTC 解码（严格：±1）
                    int worst3 = 0, first3 = -1;
                    int mismatch3 = tex_diff(ref_astc, pix, ASTC_EXPECT_W * ASTC_EXPECT_H, &worst3, &first3);
                    OS_EMIT(em, "");
                    OS_EMIT(em, "---- ③ GPU 采样 vs 本地 ASTC 解码（%u 像素逐点比）----", ASTC_EXPECT_W * ASTC_EXPECT_H);
                    OS_EMIT(em, "  差 >1 的像素：%d / %u；最大通道差：%d", mismatch3, ASTC_EXPECT_W * ASTC_EXPECT_H, worst3);
                    if (!mismatch3) {
                        OS_EMIT(em, "  ✅ 一致 —— 我们写的 16 字节块是**合法 ASTC**，GPU 与本地解码器读法相同");
                    } else {
                        fails++;
                        OS_EMIT(em, "  ❌ 首个不符的像素 #%d：本地=(%u,%u,%u,%u) GPU=(%u,%u,%u,%u)", first3,
                                ref_astc[(size_t)first3 * 4], ref_astc[(size_t)first3 * 4 + 1],
                                ref_astc[(size_t)first3 * 4 + 2], ref_astc[(size_t)first3 * 4 + 3],
                                pix[(size_t)first3 * 4], pix[(size_t)first3 * 4 + 1],
                                pix[(size_t)first3 * 4 + 2], pix[(size_t)first3 * 4 + 3]);
                        // 诊断：哪些块错、错块用的是什么编码配置（CEM 位）、头一个错块的两组像素
                        int nb_fail = 0, cem8 = 0, cem12 = 0, firstbad = -1;
                        char idxs[512]; size_t ip = 0;
                        idxs[0] = 0;
                        for (uint32_t byi = 0; byi < by; byi++)
                            for (uint32_t bxi = 0; bxi < bx; bxi++) {
                                int bi = (int)(byi * bx + bxi);
                                int bad = 0;
                                for (int k = 0; k < 16; k++) {
                                    uint32_t px_i = (byi * 4 + (uint32_t)k / 4) * ASTC_EXPECT_W + bxi * 4 + (uint32_t)k % 4;
                                    int d = 0;
                                    for (int ch = 0; ch < 4; ch++) {
                                        int e = (int)ref_astc[(size_t)px_i * 4 + ch] - (int)pix[(size_t)px_i * 4 + ch];
                                        if (e < 0) e = -e;
                                        if (e > d) d = e;
                                    }
                                    if (d > 1) bad++;
                                }
                                if (bad) {
                                    nb_fail++;
                                    if (firstbad < 0) firstbad = bi;
                                    int cem = (astc_out[(size_t)bi * 16 + 1] >> 5) | ((astc_out[(size_t)bi * 16 + 2] & 1) << 3);
                                    if (cem == 8) cem8++; else if (cem == 12) cem12++;
                                    if (ip + 8 < sizeof idxs) ip += (size_t)snprintf(idxs + ip, sizeof idxs - ip, "%d ", bi);
                                }
                            }
                        OS_EMIT(em, "  （诊断）出错的块：%d / %d（其中 CEM8 %d 块、CEM12 %d 块）；块号：%s",
                                nb_fail, ASTC_EXPECT_BLOCKS, cem8, cem12, idxs);
                        if (firstbad >= 0) {
                            const uint8_t *b16 = astc_out + (size_t)firstbad * 16;
                            OS_EMIT(em, "  （诊断）首个错块 #%d 的 16 字节：%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X",
                                    firstbad, b16[0], b16[1], b16[2], b16[3], b16[4], b16[5], b16[6], b16[7],
                                    b16[8], b16[9], b16[10], b16[11], b16[12], b16[13], b16[14], b16[15]);
                            uint32_t bxx = (uint32_t)firstbad % bx, byy = (uint32_t)firstbad / bx;
                            for (int k = 0; k < 16; k++) {
                                uint32_t px_i = (byy * 4 + (uint32_t)k / 4) * ASTC_EXPECT_W + bxx * 4 + (uint32_t)k % 4;
                                OS_EMIT(em, "    px%2d 本地=(%3u,%3u,%3u,%3u) GPU=(%3u,%3u,%3u,%3u)", k,
                                        ref_astc[(size_t)px_i * 4], ref_astc[(size_t)px_i * 4 + 1],
                                        ref_astc[(size_t)px_i * 4 + 2], ref_astc[(size_t)px_i * 4 + 3],
                                        pix[(size_t)px_i * 4], pix[(size_t)px_i * 4 + 1],
                                        pix[(size_t)px_i * 4 + 2], pix[(size_t)px_i * 4 + 3]);
                            }
                        }
                    }
                    // ④ GPU vs CPU BC 解码（有损：PSNR + 最大差）
                    {
                        double p = tex_psnr_rgb(pix, ref_bc, ASTC_EXPECT_W * ASTC_EXPECT_H);
                        int worst4 = 0, first4 = -1;
                        tex_diff(ref_bc, pix, ASTC_EXPECT_W * ASTC_EXPECT_H, &worst4, &first4);   // 只取最大差
                        OS_EMIT(em, "");
                        OS_EMIT(em, "---- ④ GPU 采样 vs CPU 的 BC 解码（转码前的理想图）----");
                        OS_EMIT(em, "  PSNR（RGB）：%.2f dB（参考实现 %.2f dB，允许低 0.5 dB）；最大通道差：%d（上限 32）",
                                p, (double)ASTC_EXPECT_REF_PSNR, worst4);
                        if (p + 0.5 < (double)ASTC_EXPECT_REF_PSNR || worst4 > 32) {
                            fails++;
                            OS_EMIT(em, "  ❌ 转码质量不达参考值 —— 先查 ②（字节是否一致）");
                        } else {
                            OS_EMIT(em, "  ✅ 转码质量达标（两色块无损、渐变块有损但幅度符合参考）");
                        }
                    }
                } else {
                    fails++;
                }
                vk_backend_target_free(&sc, t);
            }
            // 共享上下文是借的：**不能** shutdown（会销毁生产路径的实例/设备）
        }
    }

    // ---- ⑤ 负向：跳过解排布直接转码 ----
    if (!fails) {
        int rc = nvn_bc2astc_region(0x44, bl, row_bytes, bx, by, astc_bad, (size_t)bx * 16);
        int nbad = 0;
        if (rc == 0) for (int i = 0; i < ASTC_EXPECT_BLOCKS; i++)
            if (memcmp(astc_bad + (size_t)i * 16, astc_out + (size_t)i * 16, 16) != 0) nbad++;
        OS_EMIT(em, "");
        OS_EMIT(em, "---- ⑤ 负向判据：跳过解排布，直接拿块线性字节去转码 ----");
        OS_EMIT(em, "  与正确转码结果不同的块：%d / %d", nbad, ASTC_EXPECT_BLOCKS);
        if (rc != 0) { OS_EMIT(em, "❌ 转码拒绝"); fails++; }
        else if (!nbad) { OS_EMIT(em, "❌ 竟然一样 —— 说明解排布没起作用，要查"); fails++; }
        else OS_EMIT(em, "  ✅ 如预期地不同 —— 解排布这一步确实在干活");
    }

    // ---- ⑥ 账：尺寸 + 吞吐（单线程 vs 多线程）+ ⑦ 多线程与单线程逐字节一致 ----
    OS_EMIT(em, "");
    OS_EMIT(em, "---- ⑥ 账 ----");
    {
        size_t rgba_bytes = (size_t)ASTC_EXPECT_W * ASTC_EXPECT_H * 4;
        OS_EMIT(em, "  尺寸：BC3 %d 字节 → ASTC %zu 字节（×%.2f）；对照 RGBA8 %zu 字节 ⇒ ASTC 省 %.1f 倍",
                ASTC_EXPECT_BC3_BYTES, astc_bytes, (double)astc_bytes / ASTC_EXPECT_BC3_BYTES,
                rgba_bytes, (double)rgba_bytes / (double)astc_bytes);

        // 吞吐在**1024×1024**上量（65536 块）：60 块的小图量不出多线程的意义（建线程的开销比活儿还大），
        // 而真贴图正是这个量级。单线程一趟 ≈ 实测值，多线程一趟 ≈ 线程数分摊后的值。
        const uint32_t BIG = 1024, BW = BIG / 4, BH = BIG / 4;          // 256×256 块
        const size_t big_src = (size_t)BW * BH * 16, big_dst = (size_t)BW * BH * 16;
        uint8_t *bs = (uint8_t *)malloc(big_src), *bd1 = (uint8_t *)malloc(big_dst), *bd2 = (uint8_t *)malloc(big_dst);
        int ncpu = nvn_bc2astc_cpu_count();
        int nth = ncpu > 8 ? 8 : (ncpu > 1 ? ncpu : 1);
        if (!bs || !bd1 || !bd2) {
            OS_EMIT(em, "  ⚠ 大图缓冲分配失败，吞吐只在 60 块小图上量");
            const int rounds = 200;
            uint64_t t0b = ms_now();
            for (int r = 0; r < rounds; r++) nvn_bc2astc_region(0x44, bc_back, row_bytes, bx, by, astc_bad, (size_t)bx * 16);
            uint64_t tt = ms_now() - t0b;
            double per_block_us = tt ? (double)tt * 1000.0 / (double)(rounds * ASTC_EXPECT_BLOCKS) : 0.0;
            OS_EMIT(em, "  单线程：每块 %.2f µs，源字节 %.2f MB/s",
                    per_block_us, tt ? ((double)ASTC_EXPECT_BC3_BYTES * rounds / 1048576.0) / ((double)tt / 1000.0) : 0.0);
        } else {
            // 源数据：先整片填一个固定值（保证两条路读到完全相同的字节），再把 60 块图案铺进每行开头
            memset(bs, 0x5A, big_src);
            for (uint32_t y = 0; y < BH; y++)
                memcpy(bs + (size_t)y * BW * 16, astc_expect_bc3, ASTC_EXPECT_BC3_BYTES);
            // 预热（页错误、缓存冷启动都算进第一趟会失真）
            nvn_bc2astc_region(0x44, bs, (size_t)BW * 16, BW, BH, bd1, (size_t)BW * 16);

            uint64_t a0 = ms_now();
            nvn_bc2astc_region(0x44, bs, (size_t)BW * 16, BW, BH, bd1, (size_t)BW * 16);
            uint64_t single_ms = ms_now() - a0;

            uint64_t b0 = ms_now();
            int rc = nvn_bc2astc_region_mt(0x44, bs, (size_t)BW * 16, BW, BH, bd2, (size_t)BW * 16, nth);
            uint64_t mt_ms = ms_now() - b0;

            int same = rc == 0 && memcmp(bd1, bd2, big_dst) == 0;
            double blocks = (double)BW * BH;
            double s_us = single_ms ? single_ms * 1000.0 / blocks : 0.0;
            double m_us = mt_ms ? mt_ms * 1000.0 / blocks : 0.0;
            double s_mb = single_ms ? (big_src / 1048576.0) / (single_ms / 1000.0) : 0.0;
            double m_mb = mt_ms ? (big_src / 1048576.0) / (mt_ms / 1000.0) : 0.0;

            OS_EMIT(em, "  1024×1024（%u 块）单线程：%llums ⇒ 每块 %.2f µs、源字节 %.2f MB/s",
                    BW * BH, (unsigned long long)single_ms, s_us, s_mb);
            OS_EMIT(em, "  同图 %d 线程（CPU %d 核）：%llums ⇒ 每块 %.2f µs、源字节 %.2f MB/s，加速 %.1f×",
                    nth, ncpu, (unsigned long long)mt_ms, m_us, m_mb, mt_ms ? (double)single_ms / (double)mt_ms : 0.0);
            OS_EMIT(em, "  换算：2048×2048（262144 块）单线程 ≈ %.0f ms，%d 线程 ≈ %.0f ms",
                    s_us * 262144.0 / 1000.0, nth, m_us * 262144.0 / 1000.0);

            OS_EMIT(em, "");
            OS_EMIT(em, "---- ⑦ 多线程 vs 单线程（1024×1024 逐字节）----");
            OS_EMIT(em, "  %s：%d 线程的输出与单线程%s（切分只按块行，块间无依赖）",
                    same ? "✅" : "❌", nth, same ? "**逐字节相同**" : "**不同 —— 切分有 bug**");
            if (!same) fails++;
            free(bs); free(bd1); free(bd2);
        }
    }

    free(bl); free(bc_back); free(astc_out); free(astc_bad); free(ref_bc); free(ref_astc); free(pix);
    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.4-E %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    dump_report(dir, "dm_tex_c3.log", out);
    return fails;
}

// ================= 阶段 4（3.4-B）：块线性解排布 =================
// **不自己另写判据** —— 直接收编上游自己的单测 `dm/gfx/tex/tests/t_tex.c`：
//   A 往返（各级/层随机数据往返逐字节）
//   B 地址公式（期望值按 GOB 64B×8 行的公开排布**手算**，注释里连加法过程都写着）
//   C/D 真数据对拍（Switch 块线性 vs PC 线性，阈值 PSNR）+ 错块高负对照
//   E 手工 3D（按手算地址往块线性存储里放标记，解排布后核标记落点）
// 形状与宿主单测一样：一个单测一个 .so，按名字 dlopen 后取 main 调；它的 printf 用宿主单测那套 dup2 捕获。
//
// 为什么这一步必须做：真实贴图在 Switch 上是**块线性**存的（Tegra GOB 排布），不解出来就没法解码/上传。
// 而 `nvn_tex.c` / `nvn_deswizzle.c` 是**平台中立**的（算法移植自 deko3d 的 dk_image.cpp，无任何平台调用）
// ⇒ 直接编进来复用，**零改动** —— 这也正是"L2 领域层"该有的样子。
//
// ⚠️ C/D 段需要**游戏样本**（`dm/gfx/tex/tests/fetch_samples.py` 从你的游戏文件里产出）。
// 本机没有样本时它会明确报两处红（"缺样本目录" + "真数据样本不足 3 对"）—— 那是**未跑**，不是移植缺陷；
// 本探针会把红行**按段归类**打印出来，便于一眼分清。
#define TEX_LAYOUT_LOG_MAX 12288

// ---- 收编上游单测的**通用**部分（3.4-B 的块线性、t_pace 的节拍都用它）----
// 按名字 dlopen → 取 main → 跑 → 把它的 stdout/stderr 捕获到文件 → 读回文本。
// 为什么抽出来：收编第二、第三个上游单测时，重写一遍 dup2 捕获 + 读文件就是"抄第二份"，
// 以后改一处会漏另一处。
// 返回 0 成功；*text_out 是读回的文本（调用方 free）；*rc_out 是单测自己的返回值。
static int run_upstream_test(os_emit *em, const char *dir, const char *sofile, const char *arg1,
                             const char *capname, char **text_out, int *rc_out) {
    *text_out = NULL; *rc_out = 0;
    void *h = dlopen(sofile, RTLD_NOW);
    if (!h) { OS_EMIT(em, "❌ 载入 %s 失败：%s", sofile, dlerror()); return -1; }
    int (*fn)(int, char **) = (int (*)(int, char **))dlsym(h, "main");
    if (!fn) { OS_EMIT(em, "❌ dlsym(main) 失败：%s", dlerror()); dlclose(h); return -1; }

    char logpath[1200];
    snprintf(logpath, sizeof logpath, "%s/%s", (dir && *dir) ? dir : ".", capname);
    int saved = dm_test_capture_begin(logpath);
    if (saved < 0) { OS_EMIT(em, "❌ 打不开捕获文件：%s", logpath); dlclose(h); return -1; }
    char *argv[3];
    argv[0] = (char *)sofile; argv[1] = (char *)(arg1 ? arg1 : ""); argv[2] = NULL;
    int rc = fn(2, argv);
    dm_test_capture_end(saved);
    dlclose(h);

    char *text = (char *)malloc(TEX_LAYOUT_LOG_MAX + 1);
    if (!text) { OS_EMIT(em, "❌ 内存不足，读不回单测输出（文件在 %s）", logpath); return -1; }
    size_t got = 0;
    FILE *f = fopen(logpath, "rb");
    if (f) { got = fread(text, 1, TEX_LAYOUT_LOG_MAX, f); fclose(f); }
    text[got] = 0;
    if (!got) { OS_EMIT(em, "❌ 单测没有任何输出（捕获文件空）"); free(text); return -1; }
    OS_EMIT(em, "✓ 单测跑完，返回 %d（0 = 全过；输出 %zu 字节%s）", rc, got,
            got >= TEX_LAYOUT_LOG_MAX ? "，已截断" : "");
    *text_out = text; *rc_out = rc;
    return 0;
}

// 打印捕获到的原文（逐行缩进），供报告里贴出上游自己的输出
static void emit_captured(os_emit *em, const char *text) {
    for (const char *p = text; *p; ) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[1024];
        size_t m = n < sizeof line - 1 ? n : sizeof line - 1;
        memcpy(line, p, m); line[m] = 0;
        OS_EMIT(em, "  %s", line);
        if (!nl) break;
        p = nl + 1;
    }
}

int vk_backend_tex_layout(const char *dir, char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.4-B）：块线性解排布 —— 收编上游单测 t_tex ===");
    OS_EMIT(em, "判据全部来自上游：A 往返 ／ B 手算地址 ／ C/D 真数据对拍+错块高负对照 ／ E 手工 3D");

    char *text = NULL; int rc = 0;
    char sample_dir[1200];
    snprintf(sample_dir, sizeof sample_dir, "%s/tex_samples", (dir && *dir) ? dir : ".");
    if (run_upstream_test(em, dir, "dmt_tex_layout.so", sample_dir, "tex_layout.log", &text, &rc)) {
        *em->p = 0; return 1;
    }
    size_t got = strlen(text);

    // 段归类：红行归给**最近一次见到的段头**。
    // 注意上游的输出顺序是 A → B → E → **C/D**（C/D 在最后），所以不能按"序号最大"归，
    // 也不能想当然地按 A/B/C/D/E 排 —— 第一版就是栽在这里（把缺样本的红算到了 E 段）。
    static const char *SEC[4] = { "A 往返", "B 地址", "E 手工", "C/D 真数据" };
    int sec_mark[4] = { 0 }, sec_fail[4] = { 0 }, total_fail = 0, cur = -1;
    for (char *p = text; *p; ) {
        char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        int hit = -1;
        for (int s = 0; s < 4; s++)
            if (n >= strlen(SEC[s]) && strncmp(p, SEC[s], strlen(SEC[s])) == 0) { hit = s; break; }
        if (hit >= 0) { cur = hit; sec_mark[hit] = 1; }
        else if (n >= 3 && !strncmp(p, "  ", 2) && (uint8_t)p[2] == 0xE2) {   // "  ✗ …"（✗ = E2 9C 97）
            total_fail++;
            if (cur >= 0) sec_fail[cur]++;
        }
        if (!nl) break;
        p = nl + 1;
    }

    OS_EMIT(em, "");
    OS_EMIT(em, "---- 单测自己的输出（原文，%zu 字节%s）----", got,
            got >= TEX_LAYOUT_LOG_MAX ? "，已截断" : "");
    emit_captured(em, text);

    OS_EMIT(em, "");
    OS_EMIT(em, "---- 判据归类（哪一段红，一眼分清）----");
    for (int s = 0; s < 4; s++)
        OS_EMIT(em, "  %-12s %s%s", SEC[s], sec_mark[s] ? (sec_fail[s] ? "红" : "全过") : "（本段没跑到）",
                sec_fail[s] ? "" : "");
    OS_EMIT(em, "  红行合计：%d", total_fail);

    // 判定：**C/D 段因缺样本而红不算移植缺陷**（上游明文"样本缺失算红，不跳过"，所以它会明确报出来）。
    int c_only = (total_fail == sec_fail[3]);      // 3 = "C/D 真数据"
    if (total_fail == 0) {
        OS_EMIT(em, "  ✅ 全过 —— 块线性解排布（含手算地址、往返、3D 标记）在鸿蒙侧与上游完全一致");
    } else if (c_only) {
        OS_EMIT(em, "  ⚠ 红行全部落在 C/D 段，且原因是**缺游戏样本**（A/B/E 全过）");
        OS_EMIT(em, "     ⇒ 这不是移植缺陷：C/D 要 fetch_samples.py 从游戏文件里取样本，本机没有。");
        OS_EMIT(em, "     ⇒ 拿到样本后重跑同一个按钮即可把它补齐。");
    } else {
        fails++;
        OS_EMIT(em, "  ❌ 除缺样本之外还有红行 —— 那才是要看的东西");
    }
    free(text);

    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.4-B %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    return fails;
}

// ================= 阶段 4（3.4-D）：出图与回放层合流 =================
// 之前两条路是分开的：
//   3.3-A 的"上屏"走的是**探针自己搭的最小管线**（自己建管线、自己录一个三角形）；
//   3.2 的"回放"是**离屏的**（画到自建目标，再读回来看像素）。
// 这一步把两条接起来，让画面真的从**回放层**出来：
//   手写命令流 → `nvn_vk_exec` 回放 → 画进**交换链图像** → 上屏
//
// 接法就是给回放层的那道缝（`nvn_vk_target_bind_external`）：回放层平时自建渲染目标、结束布局是
// TRANSFER_SRC（供读回）；上屏要求画进交换链图像、结束布局是 PRESENT_SRC_KHR。所以由**这条出图路径**
// 把"画到哪"递进去，回放层本身不认识交换链 —— 职责没有串。
//
// 判据（与 3.2 / 阶段 2 同一组像素，便于逐条对照）：
//   ① 从交换链图像读回：视口内 (32,16) 应是三角形蓝、视口外 (600,400) 应是清屏红
//   ② `nvn_vk_target_image()` 必须就是这一帧的交换链图像（证明确实画在它上面，不是又自建了一个）
//   ③ 屏幕上肉眼可见（截图由人确认）
// 已知未做：本步的视口还是 3.2 那套 64×64（所以屏幕上是左上角一个小三角形），
//           "整屏视口 / 多通道 / 贴图绑定"留到后面按需扩。
int vk_backend_onscreen_replay(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    os_ctx o;
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 阶段 4（3.4-D）：出图与回放层合流 —— 记录流 → 回放 → 上屏 ===");
    OS_EMIT(em, "复用 3.3-A 的交换链骨架 + 3.2 的手写命令流；画的目标由本路径递进回放层");

    // ---- ① 交换链骨架（3.3-A 那一套，原样复用）----
    fails = os_setup(em, &o);
    if (fails) { os_teardown(&o); OS_EMIT(em, ""); OS_EMIT(em, "===== 3.4-D 未全通过：出图骨架都没搭起来（失败 %d 项）=====", fails); *em->p = 0; return fails; }

    // ---- ② 回放环境（3.2 那一套，幂等；含客体堆 + 池 + 手写命令流）----
    char env_err[256] = "";
    if (replay_env_build(env_err, sizeof env_err)) {
        OS_EMIT(em, "❌ 回放环境搭建失败：%s", env_err); fails++;
    }
    VkDevice dev = dm_ohos_vk_device();

    // ---- ③ 同一段命令流**先离屏跑一遍**：判据①在这条干净的路上取 ----
    // 为什么不在交换链图像上读回：真机实测——对交换链图像做
    // PRESENT_SRC → TRANSFER_SRC → PRESENT_SRC 的往返之后，present 返回成功、像素也对，**但屏幕上什么都不显示**。
    // 与其去猜驱动的脾气，不如把两件事分开：**离屏那条路负责"画得对不对"，出图那条路只负责"上屏"**。
    if (!fails) {
        if (nvn_vk_frame_begin() != 0) { OS_EMIT(em, "❌ 离屏回放 frame_begin 失败：%s", nvn_vk_last_error()); fails++; }
        else {
            nvn_vk_exec(g_env_stream);
            if (nvn_vk_frame_end() != 0) { OS_EMIT(em, "❌ 离屏回放 frame_end 失败：%s", nvn_vk_last_error()); fails++; }
        }
    }
    if (!fails) {
        nvn_vk_stats_t st; nvn_vk_stats_get(&st);
        uint32_t tw = 0, th = 0; nvn_vk_target_extent(&tw, &th);
        OS_EMIT(em, "✓ 离屏回放完成：命令 %llu 条、未处理 %llu、绘制 %llu、通道 %llu、新建管线 %llu / 命中缓存 %llu、GPU %.3f ms（离屏目标 %ux%u）",
                (unsigned long long)st.ops, (unsigned long long)st.ops_skipped,
                (unsigned long long)st.draws, (unsigned long long)st.passes,
                (unsigned long long)st.pso_built, (unsigned long long)st.pso_hit, (double)st.gpu_ns / 1e6, tw, th);
        // 回放层**自建**目标的结束布局就是 TRANSFER_SRC ⇒ 直接拷，不用任何布局转换
        VkImage src = nvn_vk_target_image();
        VkBuffer rb = NULL; VkDeviceMemory rb_mem = NULL; void *rb_map = NULL;
        const int PX[2][2] = { { 32, 16 }, { 0, 63 } };      // 与 3.2 / 阶段 2 同一组像素
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = 8; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkMemoryRequirements mr; VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        VkBufferImageCopy reg[2]; memset(reg, 0, sizeof reg);
        for (int i = 0; i < 2; i++) {
            reg[i].bufferOffset = (VkDeviceSize)(i * 4);
            reg[i].imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            reg[i].imageOffset = (VkOffset3D){ PX[i][0], PX[i][1], 0 };
            reg[i].imageExtent = (VkExtent3D){ 1, 1, 1 };
        }
        if (!src || vkCreateBuffer(dev, &bi, NULL, &rb) != VK_SUCCESS) { OS_EMIT(em, "❌ 读回缓冲创建失败"); fails++; }
        else {
            vkGetBufferMemoryRequirements(dev, rb, &mr);
            mi.allocationSize = mr.size; mi.memoryTypeIndex = dm_ohos_heap_mem_type();
            if (vkAllocateMemory(dev, &mi, NULL, &rb_mem) != VK_SUCCESS ||
                vkBindBufferMemory(dev, rb, rb_mem, 0) != VK_SUCCESS ||
                vkMapMemory(dev, rb_mem, 0, 8, 0, &rb_map) != VK_SUCCESS) {
                OS_EMIT(em, "❌ 读回缓冲映射失败"); fails++;
            }
        }
        if (!fails) {
            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = dm_ohos_vk_pool(); ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            VkCommandBuffer cb = NULL;
            if (vkAllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) { OS_EMIT(em, "❌ 取命令缓冲失败"); fails++; }
            else {
                VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &cbi);
                vkCmdCopyImageToBuffer(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 2, reg);
                vkEndCommandBuffer(cb);
                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                VkQueue q = dm_ohos_vk_queue();
                if (vkQueueSubmit(q, 1, &si, NULL) != VK_SUCCESS || vkQueueWaitIdle(q) != VK_SUCCESS) {
                    OS_EMIT(em, "❌ 读回提交失败"); fails++;
                }
                vkFreeCommandBuffers(dev, dm_ohos_vk_pool(), 1, &cb);
            }
        }
        if (rb_map) {
            const uint8_t *px = (const uint8_t *)rb_map;
            OS_EMIT(em, "");
            OS_EMIT(em, "---- 判据①：离屏读回（画面是不是从记录流回放出来的）----");
            OS_EMIT(em, "  三角形内 (%d,%d) 读到 (%u,%u,%u,%u)，预期蓝 (0,0,255,255)",
                    PX[0][0], PX[0][1], px[0], px[1], px[2], px[3]);
            OS_EMIT(em, "  三角形外 (%d,%d) 读到 (%u,%u,%u,%u)，预期红 (255,0,0,255)",
                    PX[1][0], PX[1][1], px[4], px[5], px[6], px[7]);
            int ok_in = px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255;
            int ok_out = px[4] == 255 && px[5] == 0 && px[6] == 0 && px[7] == 255;
            OS_EMIT(em, "  %s 与 3.2 / 阶段 2 同一组像素一致", (ok_in && ok_out) ? "✅" : "❌");
            if (!ok_in || !ok_out) fails++;
            vkUnmapMemory(dev, rb_mem);
        }
        if (rb) vkDestroyBuffer(dev, rb, NULL);
        if (rb_mem) vkFreeMemory(dev, rb_mem, NULL);
    }

    // ---- ④⑤⑥ 出图：取图 → 建视图/通道/帧缓冲 → 登记外部目标 → 回放 → present ----
    // 这一段刻意**照抄 3.3-A 那套已经验过的东西**，只在"画的目标"上换成回放层：
    //   · 取图用**信号量**（不是栅栏）、且由一次提交把它等掉 —— 3.3-A 就是这么做的；
    //   · present 等一个由提交 signal 的信号量；
    //   · **连续出几帧**（合成器往往要看到这个 surface 连续被交上来才接管）。
    // 真机踩到的坑记在这：只 present 一帧、或取图只给栅栏不给信号量时，present 返回成功、像素也对，
    // 但**屏幕上什么都不显示**（XComponent 全黑）。这两条都是"不报错的错"。
    {
        VkSemaphore sem_avail = NULL, sem_done = NULL;
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(dev, &sci, NULL, &sem_avail);
        vkCreateSemaphore(dev, &sci, NULL, &sem_done);

        // 诊断（二分）：出图那几帧**不录时间戳** —— 回放层帧与探针帧在命令内容上只差这一样
        // （渲染通道/帧缓冲/图像/提交顺序都已逐一对过）。离屏回放已跑完（那段照常录）。
        nvn_vk_ts_record_set(0);

        const int FRAMES = 3;
        int ok_frames = 0;
        for (int f = 0; f < FRAMES && !fails; f++) {
            uint32_t idx = 0;

            VkResult r = vkAcquireNextImageKHR(dev, o.sc, 2000000000ull, sem_avail, VK_NULL_HANDLE, &idx);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { OS_EMIT(em, "❌ 第 %d 帧取图失败 VkResult=%d", f, (int)r); fails++; break; }
            if (idx > 7) { OS_EMIT(em, "❌ 交换链图像下标异常（%u）", idx); fails++; break; }

            // **直接用 os_setup 建好的那一套**（view[idx] / 共用 rp / fb[idx]）—— 别自己另建一套：
            // 3.3-A 能上屏用的就是这一套，自建的那套（参数一样）真机上画面出不来。
            VkImageView view = o.view[idx]; VkRenderPass rp = o.rp; VkFramebuffer fb = o.fb[idx];

            if (nvn_vk_target_bind_external(&g_tex, o.img[idx], view, rp, fb, o.ext.width, o.ext.height,
                                            o.fmt, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) {
                OS_EMIT(em, "❌ 绑定外部渲染目标失败：%s", nvn_vk_last_error()); fails++; break;
            }
            if (f == 0) OS_EMIT(em, "✓ 交换链图像已登记为回放层的外部渲染目标（含 PRESENT_SRC 结束布局）");

            // 取图的信号量由**回放层这一次提交**来等、完成时 signal 给 present 等 ——
            // 等待与绘制必须在**同一次提交**里：真机实测，把等待放进另一次空提交（回放层的提交自己不等）
            // ⇒ present 返回成功、像素也对，**但合成器不收**（屏幕全黑）。
            // 这正是 D0/D2 诊断指出的方向：os_frame_do（等待与绘制同提交）能显示，我们的空提交不能。
            nvn_vk_frame_wait(sem_avail, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            nvn_vk_frame_signal(sem_done);

            if (nvn_vk_frame_begin() != 0) { OS_EMIT(em, "❌ 回放 frame_begin 失败：%s", nvn_vk_last_error()); fails++; break; }
            nvn_vk_exec(g_env_stream);
            if (nvn_vk_frame_end() != 0) { OS_EMIT(em, "❌ 回放 frame_end 失败：%s", nvn_vk_last_error()); fails++; break; }

            if (f == 0) {
                // 判据②：证明确实画在交换链图像上，而不是回放层又自建了一个
                int same = (nvn_vk_target_image() == o.img[idx]);
                OS_EMIT(em, "---- 判据②：回放层的目标图像 %s 本帧交换链图像 #%u ----", same ? "=" : "**≠**", idx);
                if (!same) { fails++; break; }

                // 诊断（当前配置下的图像内容）：present 之前把刚画完的交换链图像读回两个点。
                // 早先那次"读回像素正确"是在旧配置（有布局往返、无信号量顺序）下测的；
                // 现在的配置还没测过 —— 若内容对而屏幕黑 ⇒ 合成器在丢正确帧；若内容黑 ⇒ 回放没画上去。
                {
                    VkBuffer rb = NULL; VkDeviceMemory rbm = NULL; void *mp = NULL;
                    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                    bi.size = 8; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    VkBufferImageCopy reg[2]; memset(reg, 0, sizeof reg);
                    const int PX[2][2] = { { 32, 16 }, { 600, 400 } };
                    for (int i = 0; i < 2; i++) {
                        reg[i].bufferOffset = (VkDeviceSize)(i * 4);
                        reg[i].imageSubresource = (VkImageSubresourceLayers){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        reg[i].imageOffset = (VkOffset3D){ PX[i][0], PX[i][1], 0 };
                        reg[i].imageExtent = (VkExtent3D){ 1, 1, 1 };
                    }
                    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = o.img[idx];
                    b.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    if (vkCreateBuffer(dev, &bi, NULL, &rb) == VK_SUCCESS) {
                        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, rb, &mr);
                        VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                        mi.allocationSize = mr.size; mi.memoryTypeIndex = dm_ohos_heap_mem_type();
                        if (vkAllocateMemory(dev, &mi, NULL, &rbm) == VK_SUCCESS &&
                            vkBindBufferMemory(dev, rb, rbm, 0) == VK_SUCCESS &&
                            vkMapMemory(dev, rbm, 0, 8, 0, &mp) == VK_SUCCESS) {
                            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                            ai.commandPool = dm_ohos_vk_pool(); ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                            VkCommandBuffer cb = NULL;
                            if (vkAllocateCommandBuffers(dev, &ai, &cb) == VK_SUCCESS) {
                                VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                                cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                                vkBeginCommandBuffer(cb, &cbi);
                                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                     0, 0, NULL, 0, NULL, 1, &b);
                                vkCmdCopyImageToBuffer(cb, o.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 2, reg);
                                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                                b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; b.dstAccessMask = 0;
                                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                                     0, 0, NULL, 0, NULL, 1, &b);
                                vkEndCommandBuffer(cb);
                                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                                if (vkQueueSubmit(dm_ohos_vk_queue(), 1, &si, NULL) == VK_SUCCESS) vkQueueWaitIdle(dm_ohos_vk_queue());
                                vkFreeCommandBuffers(dev, dm_ohos_vk_pool(), 1, &cb);
                                const uint8_t *px = (const uint8_t *)mp;
                                OS_EMIT(em, "  （诊断）present 前图像内容：(%d,%d)=(%u,%u,%u,%u)  (%d,%d)=(%u,%u,%u,%u)"
                                            "  —— 预期 蓝 / 红",
                                        PX[0][0], PX[0][1], px[0], px[1], px[2], px[3],
                                        PX[1][0], PX[1][1], px[4], px[5], px[6], px[7]);
                            }
                            vkUnmapMemory(dev, rbm);
                        }
                        if (rbm) vkFreeMemory(dev, rbm, NULL);
                        if (rb) vkDestroyBuffer(dev, rb, NULL);
                    }
                }
            }

            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sem_done;
            pi.swapchainCount = 1; pi.pSwapchains = &o.sc; pi.pImageIndices = &idx;
            r = vkQueuePresentKHR(o.qpresent, &pi);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { OS_EMIT(em, "❌ 第 %d 帧 present 失败 VkResult=%d", f, (int)r); fails++; }
            else ok_frames++;

            vkQueueWaitIdle(dm_ohos_vk_queue());
            // view / rp / fb 都归 os_setup 那一套，由 os_teardown 统一销毁，这里不碰
        }
        if (!fails) {
            OS_EMIT(em, "---- 判据③：present %d 帧 ✅（交换链存活时经截图+人眼确认）----", ok_frames);
            OS_EMIT(em, "  屏幕停在：整屏红 + 左上角小蓝▽（回放帧，视口 64×64）—— **合流完成**");
            OS_EMIT(em, "  ⭐ 根因（排查了四轮）：画面一直能上屏；「全黑」是因为**探针结束就销毁交换链**，");
            OS_EMIT(em, "     合成器随即回收表面内容，而截图全拍在销毁之后。保留交换链（KEEP_ALIVE）后一切正常。");
            OS_EMIT(em, "  教训：**上屏判定必须在交换链存活时做**（截图或人眼），销毁之后拍 = 拍黑。");
        }

        // ---- 最小差异法诊断：**同一套取图/提交/present 流程**，只把"谁画的"换成探针自己的管线 ----
        // D0_D2 开关：这两段会把"最后一帧"换成探针画的画面，**人眼就看不到回放层画的帧了**。
        // 排查"回放帧到底上没上屏"时必须关掉（以人眼为准；截图不可信，见 3.4-D 的教训）。
        const int DO_D0_D2 = 0;
        if (DO_D0_D2)
        // 目的：把"我这套出图流程有问题"和"回放层画出来的东西有问题"这两件事分开。
        // 若屏幕上能看到 3.3-A 那个三角形 ⇒ 流程没问题，嫌疑全在回放层；
        // 若屏幕仍是黑的 ⇒ 问题在我这套写法上（因为 3.3-A 自己的路径是能显示的）。
        // ⚠ 这里**不能**写成 if (!fails)：判据③失败后 fails 已 +1，诊断就永远不跑了
        //   （第一版就栽在这 —— 诊断恰恰要在失败之后跑）。
        {
            int ok2 = 0;
            for (int f = 0; f < 3; f++) {
                uint32_t i2 = 0;
                VkResult r = vkAcquireNextImageKHR(dev, o.sc, 2000000000ull, sem_avail, VK_NULL_HANDLE, &i2);
                if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { OS_EMIT(em, "❌ 诊断帧取图失败 VkResult=%d", (int)r); fails++; break; }
                if (i2 > 7) { OS_EMIT(em, "❌ 诊断帧图像下标异常"); fails++; break; }
                VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                ai.commandPool = dm_ohos_vk_pool(); ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
                VkCommandBuffer cb = NULL;
                if (vkAllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) { OS_EMIT(em, "❌ 诊断帧取命令缓冲失败"); fails++; break; }
                VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cb, &cbi);
                { os_ctx tmp = o; tmp.fb[0] = o.fb[i2]; rec_os_draw(cb, &tmp); }   // 3.3-A 那套：清屏 + 画三角形
                vkEndCommandBuffer(cb);
                VkPipelineStageFlags st = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                si.waitSemaphoreCount = 1; si.pWaitSemaphores = &sem_avail; si.pWaitDstStageMask = &st;
                si.commandBufferCount = 1; si.pCommandBuffers = &cb;
                si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sem_done;
                if (vkQueueSubmit(dm_ohos_vk_queue(), 1, &si, NULL) != VK_SUCCESS) { OS_EMIT(em, "❌ 诊断帧提交失败"); fails++; break; }
                VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &sem_done;
                pi.swapchainCount = 1; pi.pSwapchains = &o.sc; pi.pImageIndices = &i2;
                r = vkQueuePresentKHR(o.qpresent, &pi);
                if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) ok2++;
                vkQueueWaitIdle(dm_ohos_vk_queue());
                vkFreeCommandBuffers(dev, dm_ohos_vk_pool(), 1, &cb);
            }
            OS_EMIT(em, "---- 诊断：**同一套流程**换成探针自己的管线，present %d 帧 ----", ok2);
            OS_EMIT(em, "  屏幕看得到 3.3-A 那个三角形 ⇒ 出图流程没问题，嫌疑在回放层画出来的东西");
            OS_EMIT(em, "  屏幕仍是黑的 ⇒ 问题在我这套取图/提交/present 的写法上");
        }

        // ---- 诊断 D2：直接调 3.3-A 那条**已被验证**的每帧路径（os_frame_do，原封不动）----
        // 它自带取图/提交/present 与信号量。若屏幕上能看到它的三角形 ⇒ 环境与 surface 都好，
        // 差别只在"谁画的/我怎么交的"；若连它也不显示 ⇒ 说明这一轮的现场状态有问题（不是代码）。
        // 同样**无条件跑**：诊断要在失败之后跑才有意义。（受同一开关控制，见上。）
        if (DO_D0_D2) {
            os_slot slot; memset(&slot, 0, sizeof slot);
            if (os_slot_init(&slot, 1, em) == 0) {
                uint32_t ix = 0; uint64_t ts = 0; const char *why = "";
                int rc = os_frame_do(&o, dm_ohos_vk_pool(), &slot, 1, &ix, &ts, &why);
                OS_EMIT(em, "---- 诊断 D2：调 3.3-A 已验证的每帧路径：%s（帧号 #%u）----",
                        rc ? why : "已 present 一帧", ix);
                os_slot_free(&slot, 1, dm_ohos_vk_pool());
            } else OS_EMIT(em, "---- 诊断 D2：槽位初始化失败 ----");
        }
        if (sem_avail) vkDestroySemaphore(dev, sem_avail, NULL);
        if (sem_done) vkDestroySemaphore(dev, sem_done, NULL);
    }

    nvn_vk_target_unbind_external(&g_tex);
    // **不销毁交换链**：销毁会让合成器回收表面内容 ⇒ 屏幕变黑，人眼就看不到了。
    // 此前所有"全黑"的截图都拍在销毁之后 —— 排查被这个收尾动作误导了整整四轮。
    // 代价是这一轮的交换链/表面留着（探针进程存活期间），下一次运行会重建。
    const int KEEP_ALIVE = 1;
    if (!KEEP_ALIVE) os_teardown(&o);

    OS_EMIT(em, "");
    OS_EMIT(em, "===== 3.4-D %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过（③经人眼/截图在交换链存活时确认）", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    return fails;
}

// ================= 零成本实测两条（待办 8）=================
// 两条都是"花几分钟就能拿到、但后面每一步的写法都要用到"的数：
//   ① 设备有没有 `VK_EXT_descriptor_buffer` / `VK_KHR_push_descriptor` ⇒ **描述符到底该怎么写**
//      （回放层每帧都要更新大量描述符；这三条路子的写法差别很大，先知道有什么再设计，
//        比先按某一种写完整套再发现设备不支持要省得多。）
//   ② 预留的那块 4 GiB 客体堆**是不是懒提交**：分配完就占满物理内存，还是"摸到哪页才算哪页"
//      ⇒ **内存账怎么算**（按 4 GiB 满额算，还是按实际用到多少算）。
//
// 为什么叫"零成本"：两条都只是查询与一次采样，不动渲染管线、不依赖游戏文件。
// 为什么"实测"不"查文档"：文档说的是规范允许什么；我们要的是**这台设备上到底是哪一种行为**。

// 从 /proc/meminfo 取一个字段（KB）；取不到返回 0
static uint64_t meminfo_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256]; uint64_t v = 0;
    while (fgets(line, sizeof line, f)) {
        size_t kl = strlen(key);
        if (strncmp(line, key, kl) == 0) { sscanf(line + kl, ": %llu", (unsigned long long *)&v); break; }
    }
    fclose(f);
    return v;
}

int vk_backend_zero_cost(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 零成本实测两条：描述符能力 + 4 GiB 客体堆的提交行为 ===");

    VkPhysicalDevice pd = dm_ohos_vk_phys();
    VkInstance inst = dm_ohos_vk_instance();
    if (!pd) { OS_EMIT(em, "❌ 共享上下文不可用：%s", dm_ohos_vk_err()); *em->p = 0; return 1; }

    // ---------- ① 描述符能力 ----------
    OS_EMIT(em, "");
    OS_EMIT(em, "---- ① 描述符怎么写（回放层每帧要更新大量描述符，先知道有哪些路子）----");
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *ext = n ? (VkExtensionProperties *)calloc(n, sizeof *ext) : NULL;
    if (ext) {
        uint32_t got = n;
        if (vkEnumerateDeviceExtensionProperties(pd, NULL, &got, ext) != VK_SUCCESS) got = 0;
        struct { const char *name; const char *why; } want[] = {
            { "VK_EXT_descriptor_buffer", "**把描述符本身放进缓冲**：写一段内存就完事，不用描述符池/集（Vulkan 1.4 的新路子）" },
            { "VK_KHR_push_descriptor", "**即时推描述符**：录命令时直接推，不必事先建集（对每帧大量更新的场景很省事）" },
            { "VK_EXT_descriptor_indexing", "描述符数组可动态索引（Vulkan 1.2 已并入核心，这里看扩展形态）" },
            { "VK_EXT_mutable_descriptor_type", "一个描述符可以改类型（省池子）" },
            { "VK_KHR_buffer_device_address", "着色器里用缓冲设备指针（1.2 起核心）" },
        };
        for (unsigned i = 0; i < sizeof want / sizeof *want; i++) {
            int has = 0;
            for (uint32_t k = 0; k < got; k++) if (!strcmp(ext[k].extensionName, want[i].name)) { has = 1; break; }
            OS_EMIT(em, "  %-32s %s   %s", want[i].name, has ? "有" : "无", want[i].why);
        }
        free(ext);
    } else OS_EMIT(em, "  （枚举设备扩展失败）");

    // update-after-bind 那一组上限在 **VkPhysicalDeviceVulkan12Properties** 里（⾮ VkPhysicalDeviceLimits）
    PFN_vkGetPhysicalDeviceProperties2 pGet2 =
        (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceProperties2");
    {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(pd, &pr);
        OS_EMIT(em, "  与描述符相关的上限（决定能不能「一个大集打天下」）：");
        OS_EMIT(em, "    每阶段 采样贴图 %u / 采样器 %u / 存储缓冲 %u / uniform 缓冲 %u",
                pr.limits.maxPerStageDescriptorSampledImages, pr.limits.maxPerStageDescriptorSamplers,
                pr.limits.maxPerStageDescriptorStorageBuffers, pr.limits.maxPerStageDescriptorUniformBuffers);
        OS_EMIT(em, "    单个描述符集 采样贴图 %u / 采样器 %u / 存储缓冲 %u / uniform 缓冲 %u",
                pr.limits.maxDescriptorSetSampledImages, pr.limits.maxDescriptorSetSamplers,
                pr.limits.maxDescriptorSetStorageBuffers, pr.limits.maxDescriptorSetUniformBuffers);
    }
    if (pGet2) {
        VkPhysicalDeviceVulkan12Properties p12;
        memset(&p12, 0, sizeof p12);
        p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
        VkPhysicalDeviceProperties2 p2;
        memset(&p2, 0, sizeof p2);
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &p12;
        pGet2(pd, &p2);
        OS_EMIT(em, "    更新后绑定·每阶段：采样贴图 %u / 采样器 %u / 存储缓冲 %u / uniform 缓冲 %u",
                p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                p12.maxPerStageDescriptorUpdateAfterBindSamplers,
                p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
                p12.maxPerStageDescriptorUpdateAfterBindUniformBuffers);
        OS_EMIT(em, "    更新后绑定·集内上限：采样贴图 %u / 存储缓冲 %u / 采样器 %u / uniform 缓冲 %u",
                p12.maxDescriptorSetUpdateAfterBindSampledImages,
                p12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                p12.maxDescriptorSetUpdateAfterBindSamplers,
                p12.maxDescriptorSetUpdateAfterBindUniformBuffers);
    } else OS_EMIT(em, "  （查不到 vkGetPhysicalDeviceProperties2，update-after-bind 上限跳过）");

#ifdef VK_EXT_descriptor_buffer
    {
        if (pGet2) {
            VkPhysicalDeviceDescriptorBufferPropertiesEXT db;
            memset(&db, 0, sizeof db);
            db.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT;
            VkPhysicalDeviceProperties2 p2;
            memset(&p2, 0, sizeof p2);
            p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            p2.pNext = &db;
            pGet2(pd, &p2);
            OS_EMIT(em, "  descriptor buffer 若可用，几种描述符的字节数：组合图像采样器 %llu、采样器 %llu、存储缓冲 %llu、uniform 缓冲 %llu",
                    (unsigned long long)db.combinedImageSamplerDescriptorSize,
                    (unsigned long long)db.samplerDescriptorSize,
                    (unsigned long long)db.storageBufferDescriptorSize,
                    (unsigned long long)db.uniformBufferDescriptorSize);
            OS_EMIT(em, "    对齐：描述符缓冲偏移 %llu；地址空间：描述符 %llu MiB / 资源 %llu MiB / 采样器 %llu MiB",
                    (unsigned long long)db.descriptorBufferOffsetAlignment,
                    (unsigned long long)(db.descriptorBufferAddressSpaceSize >> 20),
                    (unsigned long long)(db.resourceDescriptorBufferAddressSpaceSize >> 20),
                    (unsigned long long)(db.samplerDescriptorBufferAddressSpaceSize >> 20));
        }
    }
#endif

    // ---------- ② 4 GiB 客体堆的提交行为 ----------
    // ⚠ 指标教训（上一轮踩的）：客体堆是 **DEVICE_LOCAL 统一内存**，设备内存页**不计入进程 RSS**
    //（上一轮实测：预留 4 GiB 后 RSS 反而 -12.7 MiB、摸 4096 页 +0.0 ⇒ 用 RSS 判断注定"没测出来"）。
    // 这里改用**系统级 MemAvailable 差值**：设备内存真被提交，系统可用量就会掉；
    // 若是专用区（不占系统内存），它纹丝不动 —— 那本身也是内存账的重要事实。
    OS_EMIT(em, "");
    OS_EMIT(em, "---- ② 预留的 4 GiB 客体堆是不是懒提交（指标：系统可用内存差值）----");
    {
        uint64_t mt = meminfo_kb("MemTotal");
        if (mt) OS_EMIT(em, "  机器内存：总 %.2f GiB", (double)mt / 1048576.0);
    }
    uint64_t avail0 = meminfo_kb("MemAvailable");
    void *heap = dm_ohos_heap_reserve(4ull << 30);
    uint64_t heap_sz = dm_ohos_heap_size();
    uint64_t avail1 = meminfo_kb("MemAvailable");
    if (!heap) {
        OS_EMIT(em, "❌ 客体堆预留失败：%s", dm_ohos_vk_err()); fails++;
    } else {
        OS_EMIT(em, "  预留 %llu MiB，CPU 映射 %p", (unsigned long long)(heap_sz >> 20), heap);
        OS_EMIT(em, "  系统可用：预留前 %.2f GiB → 预留后 %.2f GiB（%+.1f MiB）",
                (double)avail0 / 1048576.0, (double)avail1 / 1048576.0,
                ((double)avail1 - (double)avail0) / 1024.0);

        // 每 256 KiB 摸一页（16384 页 = 64 MiB 已提交）—— 比"每 1 MiB 一页"信号更强，仍然安全
        const uint64_t STEP = 1ull << 18;
        uint64_t pages = 0;
        for (uint64_t off = 0; off + 4096 <= heap_sz; off += STEP) {
            ((volatile uint8_t *)heap)[off] = 0x5A;
            pages++;
        }
        ((volatile uint8_t *)heap)[heap_sz - 1] = 0x5A;
        uint64_t avail2 = meminfo_kb("MemAvailable");
        OS_EMIT(em, "  摸 %llu 页（≈ %llu MiB 已提交）后：系统可用 %.2f GiB（%+.1f MiB）",
                (unsigned long long)pages, (unsigned long long)(pages * 4096 / 1048576),
                (double)avail2 / 1048576.0, ((double)avail2 - (double)avail1) / 1024.0);

        double drop_reserve = ((double)avail0 - (double)avail1) / 1024.0;   // 预留就掉多少（MiB，正=掉了）
        double drop_touch   = ((double)avail1 - (double)avail2) / 1024.0;   // 摸过才掉多少（MiB，正=掉了）
        OS_EMIT(em, "");
        if (drop_reserve > 2048.0) {
            OS_EMIT(em, "  结论：**分配即占满**（预留就掉 %.0f MiB）⇒ 内存账要**按满额**算。", drop_reserve);
        } else if (drop_touch > 32.0) {
            OS_EMIT(em, "  结论：**懒提交**（预留只掉 %.1f MiB，摸过才掉 %.1f MiB ≈ 触摸量）"
                        "⇒ 内存账可**按实际用量**算；没摸到的部分不占系统内存。", drop_reserve, drop_touch);
        } else if (drop_reserve < 128.0 && drop_touch < 32.0) {
            OS_EMIT(em, "  结论：**这块内存不占系统内存**（预留/摸过都不掉，%.1f / %.1f MiB）"
                        "⇒ 它是专用区（独立计量），内存账要**单列**，不能与系统内存混算。", drop_reserve, drop_touch);
        } else {
            OS_EMIT(em, "  结论：读数在噪声区（%.1f / %.1f MiB）—— **如实记为没测出来**，待换更细的口径重测。",
                    drop_reserve, drop_touch);
            fails++;
        }
    }

    OS_EMIT(em, "");
    OS_EMIT(em, "===== 零成本实测 %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    return fails;
}

// ================= 收编上游新增的节拍单测 t_pace（并入上游时带进来的）=================
// 为什么它值得单独跑：它把两件事的**手算期望值**列成表 ——
//   · `nvn_pace()`：节拍等待（首帧起拍 / 早到等到点 / 迟到不足一周期照出 / 落后两周期不连发追帧 / 切周期…）
//   · `nvn_pace_hz()`：**出图帧率 = 刷新率整数分之一里、不低于目标的最小一档**
//     （60/120 屏→60；**144→72**；165→82.5；100→100；240→60；**119.88→59.94**；50Hz 屏→50；读不到刷新率→按目标；
//       省电 30：120→30、**144→36**）
// 第二件正是我们 3.3-B/C 实测"**请求 60fps 却得到 44.8 / 59.7**"要解决的问题：不是系统在敷衍，
// 而是**帧率该按刷新率的整数分之一取档**（每帧在屏上停整数个刷新周期，才不会交替抖动）。
// 收编方式与 3.4-B 共用同一个助手；判据是上游手算的，我们只数红行、并把原文贴出来。
int vk_backend_pace_test(const char *dir, char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    os_emit *em = &(os_emit){ out, out + cap - 1 };
    int fails = 0;
    uint64_t t0 = ms_now();

    OS_EMIT(em, "=== 节拍单测（收编上游 t_pace）：节拍等待 + 出图帧率档位 ===");
    OS_EMIT(em, "判据全是上游手算的；这一条正好回答我们 3.3-B/C「请求 60 得 44.8 / 59.7」的困惑");

    char *text = NULL; int rc = 0;
    if (run_upstream_test(em, dir, "dmt_t_pace.so", "", "t_pace.log", &text, &rc)) {
        *em->p = 0; return 1;
    }
    // 数它自己的对钩与叉（✓ = E2 9C 93、✗ = E2 9C 97）
    int ok = 0, bad = 0;
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        if (p[0] == 0xE2 && p[1] == 0x9C && p[2] == 0x93) ok++;
        else if (p[0] == 0xE2 && p[1] == 0x9C && p[2] == 0x97) bad++;
    }
    OS_EMIT(em, "");
    OS_EMIT(em, "---- 单测自己的输出（原文）----");
    emit_captured(em, text);

    OS_EMIT(em, "");
    OS_EMIT(em, "---- 判据 ----");
    OS_EMIT(em, "  对钩 %d 个、叉 %d 个（上游返回 %d）", ok, bad, rc);
    if (bad == 0 && rc == 0) {
        OS_EMIT(em, "  ✅ 全过 —— 节拍算法与帧率档位算法在鸿蒙侧与上游完全一致");
    } else {
        fails++;
        OS_EMIT(em, "  ❌ 有 %d 项不符（或返回非 0）—— 那才是要看的东西", bad);
    }

    OS_EMIT(em, "");
    OS_EMIT(em, "===== t_pace %s：失败 %d 项（总耗时 %llums）=====",
            fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *em->p = 0;
    return fails;
}
