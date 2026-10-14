// 离屏 Vulkan 冒烟后端的实现。接口见 vk_backend.h。
// Implementation of the offscreen Vulkan smoke backend; see vk_backend.h.
//
// 阶段 1 只用 transfer 通道：ClearColorImage + CopyImageToBuffer。
// 每一步 Vulkan 调用都检查返回值；失败时把"哪个调用、错误码"写进调用方给的 err 缓冲——
// 真机上排错就靠这个，不给"初始化失败了"这种没信息量的话。
// Every Vulkan call is checked; failures carry the call and the result code.
#include "vk_backend.h"

#include "dm_log.h"
#include "tri_shaders.h"

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
    // dynamic rendering is a Vulkan 1.3 core feature; without enabling it here vkCmdBeginRendering is invalid.
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
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
