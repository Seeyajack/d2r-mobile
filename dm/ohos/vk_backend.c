// 离屏 Vulkan 冒烟后端的实现。接口见 vk_backend.h。
// Implementation of the offscreen Vulkan smoke backend; see vk_backend.h.
//
// 阶段 1 只用 transfer 通道：ClearColorImage + CopyImageToBuffer。
// 每一步 Vulkan 调用都检查返回值；失败时把"哪个调用、错误码"写进调用方给的 err 缓冲——
// 真机上排错就靠这个，不给"初始化失败了"这种没信息量的话。
// Every Vulkan call is checked; failures carry the call and the result code.
#include "vk_backend.h"

#include "dm_log.h"
#include "nvn_vk.h"
#include "tri_shaders.h"
#include "vk_ctx_ohos.h"

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
