// 离屏 Vulkan 冒烟后端的实现。接口见 vk_backend.h。
// Implementation of the offscreen Vulkan smoke backend; see vk_backend.h.
//
// 阶段 1 只用 transfer 通道：ClearColorImage + CopyImageToBuffer。
// 每一步 Vulkan 调用都检查返回值；失败时把"哪个调用、错误码"写进调用方给的 err 缓冲——
// 真机上排错就靠这个，不给"初始化失败了"这种没信息量的话。
// Every Vulkan call is checked; failures carry the call and the result code.
#include "vk_backend.h"

#include "dm_log.h"
#include "nvn_obj.h"       // 3.2 手写命令流要用它的记录格式（op_hdr / p_*）
#include "nvn_vk.h"
#include "tri_shaders.h"
#include "vk_ctx_ohos.h"
#include "vk_surface_ohos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

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

// ================= 阶段 4（3.3-A）：出图 —— 屏幕上出画面 =================
// ArkUI 的 XComponent 给出 OHNativeWindow → VkSurfaceKHR → swapchain → 画 → present。
// 这是"离屏验证"与"真上屏"的分界线：前面所有判据都是读回像素，这里第一次让画面**出现在屏幕上**。
//
// 判据：① 表面能力 / 格式 / 呈现模式逐个报出（不猜）；② 60 帧 present 间隔统计（节拍稳不稳）。
// 画面本身用截图做视觉确认（真机上肉眼可辨）。
typedef struct {
    VkSurfaceKHR   surface;
    VkSwapchainKHR sc;
    VkFormat       fmt; VkColorSpaceKHR cs; VkExtent2D ext;
    uint32_t       nimg; VkImage img[8]; VkImageView view[8]; VkFramebuffer fb[8];
    VkRenderPass   rp; VkPipeline pl; VkPipelineLayout lay;
    VkBuffer       vb; VkDeviceMemory vbm;
    VkSemaphore    sem_avail, sem_done;
    VkQueue        qpresent;          // 负责 present 的队列（真机：图形族不支持）
} os_ctx;

static void os_teardown(os_ctx *o) {
    VkDevice dev = dm_ohos_vk_device();
    if (!dev) return;
    if (o->sem_avail) vkDestroySemaphore(dev, o->sem_avail, NULL);
    if (o->sem_done) vkDestroySemaphore(dev, o->sem_done, NULL);
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

int vk_backend_onscreen(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    char *p = out, *end = out + cap - 1;
    int fails = 0;
    uint64_t t0 = ms_now();
    os_ctx o; memset(&o, 0, sizeof o);
    #define EMIT9(...) do { int _n = snprintf(p, (size_t)(end - p) + 1, __VA_ARGS__); \
                            if (_n > 0) { p += _n; if (p > end) p = end; } \
                            dm_log_printf(__VA_ARGS__); } while (0)

    EMIT9("=== 阶段 4（3.3-A）：出图 —— 屏幕上出画面 ===");
    EMIT9("XComponent 窗口 → VkSurfaceKHR → swapchain → 画 → present");

    char err[256] = "";
    uint32_t win_w = 0, win_h = 0;
    if (!dm_ohos_surface_window() || dm_ohos_surface_size(&win_w, &win_h) != 0) {
        EMIT9("❌ 还没拿到 XComponent 窗口（页面里要有 XComponent，且 surface 回调已到）");
        EMIT9("===== 失败 ====="); *p = 0; return 1;
    }
    EMIT9("XComponent 窗口：%ux%u", win_w, win_h);

    if (dm_ohos_surface_create(&o.surface, err, sizeof err) != 0) {
        EMIT9("❌ %s", err); EMIT9("===== 失败 ====="); *p = 0; return 1;
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
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, f, o.surface, &pres);
            const char *what = (qsr[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "图形" :
                               (qsr[f].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "计算" : "传输";
            EMIT9("  队列族 %u：%s，队列数 %u，present %s", f, what, qsr[f].queueCount, pres ? "支持" : "不支持");
            if (pres && qf == 0xFFFFFFFFu) qf = f;
        }
        free(qsr);
    }
    if (qf == 0xFFFFFFFFu) {
        EMIT9("❌ 没有一个队列族支持 present —— 出图这条路走不通（要查 VK_OHOS_surface / 窗口是否有效）");
        fails++; EMIT9("===== 失败 ====="); os_teardown(&o); *p = 0; return fails;
    }
    for (uint32_t k = 0; k < dm_ohos_vk_qfam_count(); k++)
        if (dm_ohos_vk_qfam_at(k) == qf) o.qpresent = dm_ohos_vk_queue_at(k);
    EMIT9("✅ 提交族 %u（渲染仍走主队列，命令缓冲跨族提交是非法的）", qf);
    if (!o.qpresent) { EMIT9("❌ 拿不到提交族的队列"); fails++; }
    if (fails) { EMIT9("===== 失败 ====="); os_teardown(&o); *p = 0; return fails; }

    // ---- 表面能力 / 格式 / 呈现模式：逐个报出，不猜 ----
    VkSurfaceCapabilitiesKHR caps; memset(&caps, 0, sizeof caps);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, o.surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, o.surface, &nf, NULL);
    VkSurfaceFormatKHR *fmts = calloc(nf ? nf : 1, sizeof *fmts);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, o.surface, &nf, fmts);
    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, o.surface, &npm, NULL);
    VkPresentModeKHR *pms = calloc(npm ? npm : 1, sizeof *pms);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, o.surface, &npm, pms);
    EMIT9("表面：图像数 %u..%u，当前范围 %ux%u，变换 %#x，支持的用法 %#x",
          caps.minImageCount, caps.maxImageCount, caps.currentExtent.width, caps.currentExtent.height,
          (unsigned)caps.supportedTransforms, (unsigned)caps.supportedUsageFlags);
    for (uint32_t i = 0; i < nf && i < 6; i++)
        EMIT9("  格式 %u：VkFormat %d / 色彩空间 %d", i, (int)fmts[i].format, (int)fmts[i].colorSpace);
    for (uint32_t i = 0; i < npm && i < 6; i++)
        EMIT9("  呈现模式 %u：%d（0=立即 1=信箱 2=队列 3=三重缓冲）", i, (int)pms[i]);

    // 选：优先 RGBA8/BGRA8 非 sRGB，否则用第一个；呈现模式优先 FIFO（稳定 vsync）
    o.fmt = VK_FORMAT_UNDEFINED; o.cs = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            o.fmt = fmts[i].format; o.cs = fmts[i].colorSpace; break;
        }
    if (o.fmt == VK_FORMAT_UNDEFINED && nf) { o.fmt = fmts[0].format; o.cs = fmts[0].colorSpace; }
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < npm; i++) if (pms[i] == VK_PRESENT_MODE_FIFO_KHR) { pm = pms[i]; break; }
    if (nf == 0 || npm == 0) { EMIT9("❌ 表面没报格式或呈现模式"); fails++; }
    free(fmts); free(pms);
    if (fails) { EMIT9("===== 失败 ====="); os_teardown(&o); *p = 0; return fails; }

    o.ext.width = caps.currentExtent.width;
    o.ext.height = caps.currentExtent.height;
    if (o.ext.width == 0xFFFFFFFFu || !o.ext.width) o.ext.width = win_w;
    if (o.ext.height == 0xFFFFFFFFu || !o.ext.height) o.ext.height = win_h;
    EMIT9("选用：VkFormat %d，呈现模式 %d，交换链 %ux%u", (int)o.fmt, (int)pm, o.ext.width, o.ext.height);

    // ---- swapchain ----
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    {
        VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        ci.surface = o.surface; ci.minImageCount = want; ci.imageFormat = o.fmt; ci.imageColorSpace = o.cs;
        ci.imageExtent = o.ext; ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = pm; ci.clipped = VK_TRUE; ci.oldSwapchain = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(dev, &ci, NULL, &o.sc);
        if (r != VK_SUCCESS) { EMIT9("❌ vkCreateSwapchainKHR 失败 VkResult=%d", (int)r); fails++; }
    }
    if (fails) { EMIT9("===== 失败 ====="); os_teardown(&o); *p = 0; return fails; }
    o.nimg = 0;
    vkGetSwapchainImagesKHR(dev, o.sc, &o.nimg, NULL);
    if (o.nimg > 8) o.nimg = 8;
    vkGetSwapchainImagesKHR(dev, o.sc, &o.nimg, o.img);
    EMIT9("交换链：%u 张图像", o.nimg);

    // ---- 通道：结束布局 = PRESENT_SRC（可以直接 present）----
    {
        VkAttachmentDescription att; memset(&att, 0, sizeof att);
        att.format = o.fmt; att.samples = VK_SAMPLE_COUNT_1_BIT;
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
        if (vkCreateRenderPass(dev, &rp, NULL, &o.rp) != VK_SUCCESS) { EMIT9("❌ vkCreateRenderPass 失败"); fails++; }
    }
    for (uint32_t i = 0; i < o.nimg && !fails; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = o.img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = o.fmt;
        vi.subresourceRange = (VkImageSubresourceRange){ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(dev, &vi, NULL, &o.view[i]) != VK_SUCCESS) { EMIT9("❌ 建图像视图 %u 失败", i); fails++; break; }
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = o.rp; fi.attachmentCount = 1; fi.pAttachments = &o.view[i];
        fi.width = o.ext.width; fi.height = o.ext.height; fi.layers = 1;
        if (vkCreateFramebuffer(dev, &fi, NULL, &o.fb[i]) != VK_SUCCESS) { EMIT9("❌ 建帧缓冲 %u 失败", i); fails++; break; }
    }
    if (fails) { EMIT9("===== 失败 ====="); os_teardown(&o); *p = 0; return fails; }

    // ---- 管线（用阶段 2 那两条 SPIR-V）----
    {
        VkShaderModule vm = make_module(dev, dm_spv_vert, dm_spv_vert_n, err, sizeof err);
        VkShaderModule fm = vm ? make_module(dev, dm_spv_frag, dm_spv_frag_n, err, sizeof err) : NULL;
        if (!vm || !fm) { EMIT9("❌ 建着色器模块失败：%s", err); fails++; }
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
            if (vkCreatePipelineLayout(dev, &li, NULL, &o.lay) != VK_SUCCESS) { EMIT9("❌ 建管线布局失败"); fails++; }
            else {
                VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
                gi.stageCount = 2; gi.pStages = st;
                gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia;
                gi.pViewportState = &vps; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
                gi.pDepthStencilState = &dss; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
                gi.layout = o.lay; gi.renderPass = o.rp;
                if (vkCreateGraphicsPipelines(dev, NULL, 1, &gi, NULL, &o.pl) != VK_SUCCESS) {
                    EMIT9("❌ vkCreateGraphicsPipelines 失败"); fails++;
                }
            }
            vkDestroyShaderModule(dev, vm, NULL);
            vkDestroyShaderModule(dev, fm, NULL);
        }
    }

    // ---- 顶点缓冲 + 信号量 ----
    if (!fails) {
        const float verts[6] = { -1.f, -1.f,  1.f, -1.f,  0.f, 1.f };
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = sizeof verts; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (vkCreateBuffer(dev, &bi, NULL, &o.vb) != VK_SUCCESS) { EMIT9("❌ 建顶点缓冲失败"); fails++; }
        else {
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, o.vb, &mr);
            VkMemoryAllocateInfo mi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mi.allocationSize = mr.size; mi.memoryTypeIndex = dm_ohos_heap_mem_type();
            void *mp = NULL;
            if (!(mr.memoryTypeBits & (1u << dm_ohos_heap_mem_type()))) { EMIT9("❌ 顶点缓冲的 memoryTypeBits 不含统一内存类型"); fails++; }
            else if (vkAllocateMemory(dev, &mi, NULL, &o.vbm) != VK_SUCCESS) { EMIT9("❌ 分配顶点缓冲内存失败"); fails++; }
            else if (vkBindBufferMemory(dev, o.vb, o.vbm, 0) != VK_SUCCESS) { EMIT9("❌ 绑定顶点缓冲失败"); fails++; }
            else if (vkMapMemory(dev, o.vbm, 0, sizeof verts, 0, &mp) != VK_SUCCESS) { EMIT9("❌ 映射顶点缓冲失败"); fails++; }
            else { memcpy(mp, verts, sizeof verts); vkUnmapMemory(dev, o.vbm); }
        }
    }
    if (!fails) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(dev, &si, NULL, &o.sem_avail) != VK_SUCCESS ||
            vkCreateSemaphore(dev, &si, NULL, &o.sem_done) != VK_SUCCESS) {
            EMIT9("❌ 建信号量失败"); fails++;
        }
    }
    if (fails) { EMIT9("===== 失败 ====="); os_teardown(&o); *p = 0; return fails; }

    // ---- 出 60 帧，量 present 间隔 ----
    {
        const int FRAMES = 60;
        uint64_t best = ~0ull, worst = 0, sum = 0; int got = 0; uint64_t prev = 0;
        VkQueue q = dm_ohos_vk_queue();
        VkCommandPool pool = dm_ohos_vk_pool();
        for (int f = 0; f < FRAMES && !fails; f++) {
            uint32_t idx = 0;
            VkResult r = vkAcquireNextImageKHR(dev, o.sc, UINT64_MAX, o.sem_avail, VK_NULL_HANDLE, &idx);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { EMIT9("❌ 第 %d 帧取图失败 VkResult=%d", f, (int)r); fails++; break; }
            if (idx > 7) idx = 0;
            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) { EMIT9("❌ 取命令缓冲失败"); fails++; break; }
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cb, &bi);
            {
                os_ctx tmp = o; tmp.fb[0] = o.fb[idx];     // 本帧用第 idx 张
                rec_os_draw(cb, &tmp);
            }
            vkEndCommandBuffer(cb);
            VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1; si.pWaitSemaphores = &o.sem_avail; si.pWaitDstStageMask = &wait;
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            si.signalSemaphoreCount = 1; si.pSignalSemaphores = &o.sem_done;
            if (vkQueueSubmit(q, 1, &si, NULL) != VK_SUCCESS) { EMIT9("❌ 第 %d 帧提交失败", f); fails++; break; }
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &o.sem_done;
            pi.swapchainCount = 1; pi.pSwapchains = &o.sc; pi.pImageIndices = &idx;
            uint64_t ts = ns_now2();
            r = vkQueuePresentKHR(o.qpresent, &pi);
            vkQueueWaitIdle(q);                            // 探针：每帧等干净，测的是"节拍上限"
            vkFreeCommandBuffers(dev, pool, 1, &cb);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { EMIT9("❌ 第 %d 帧 present 失败 VkResult=%d", f, (int)r); fails++; break; }
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
            EMIT9("present %d 帧：间隔 最小 %.2f ms / 平均 %.2f ms / 最大 %.2f ms（约 %.1f fps）",
                  got, (double)best / 1e6, avg / 1e6, (double)worst / 1e6, 1e9 / avg);
            if (avg > 0 && (double)worst > avg * 2.0)
                EMIT9("⚠ 最大间隔是平均的两倍以上 —— 节拍有抖动（下一步接 OH_DisplaySoloist 再看）");
            else EMIT9("✅ 节拍稳定（最大间隔未超平均两倍）");
        } else { EMIT9("❌ 一帧都没出成"); fails++; }
    }

    os_teardown(&o);
    EMIT9("");
    EMIT9("===== 3.3-A %s：失败 %d 项（总耗时 %llums）=====",
          fails ? "未全通过" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    if (!fails) EMIT9("结论：XComponent → VkSurfaceKHR → swapchain → present 这条链通了；屏幕上应能看到三角形");
    *p = 0;
    #undef EMIT9
    return fails;
}
