// 离屏 Vulkan 冒烟后端的实现。接口见 vk_backend.h。
// Implementation of the offscreen Vulkan smoke backend; see vk_backend.h.
//
// 阶段 1 只用 transfer 通道：ClearColorImage + CopyImageToBuffer。
// 每一步 Vulkan 调用都检查返回值；失败时把"哪个调用、错误码"写进调用方给的 err 缓冲——
// 真机上排错就靠这个，不给"初始化失败了"这种没信息量的话。
// Every Vulkan call is checked; failures carry the call and the result code.
#include "vk_backend.h"

#include "dm_log.h"

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
    // 阶段 1 不开任何 feature：clear/copy 用不到。阶段 3 的 dynamic rendering 到时候再开。
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
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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

// 布局推进：每次 clear 用 UNDEFINED → TRANSFER_DST → TRANSFER_SRC。
// oldLayout 用 UNDEFINED 是合法的（内容作废）——清屏本来就不要旧内容，
// 于是"连续多次 clear"不需要记上一次的布局。
static void layout_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                           VkAccessFlagBits src, VkAccessFlagBits dst) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.image = img;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    b.oldLayout = from; b.newLayout = to;
    b.srcAccessMask = src; b.dstAccessMask = dst;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
}

typedef struct { vk_rt *t; VkClearColorValue cv; } clear_pack;

static void rec_clear(VkCommandBuffer cb, void *ud) {
    clear_pack *p = (clear_pack *)ud;
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    layout_barrier(cb, p->t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   0, VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdClearColorImage(cb, p->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &p->cv, 1, &rng);
    layout_barrier(cb, p->t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
}

int vk_backend_clear(vk_ctx *ctx, vk_rt *rt, float r, float g, float b, float a) {
    clear_pack p = { rt, { { { r, g, b, a } } } };
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
        EMIT("%s 清成%s后读回：%u 像素中 %llu 个符合预期（%llums）",
             bad ? "❌" : "✅", steps[s].name, t->w * t->h, (unsigned long long)bad,
             (unsigned long long)(ms_now() - a));
        if (bad) fails++;
    }
    free(pix);
    vk_backend_target_free(c, t);
    vk_backend_shutdown(c);

    EMIT("===== 冒烟%s：失败 %d 项（总耗时 %llums）=====",
         fails ? "失败" : "全部通过", fails, (unsigned long long)(ms_now() - t0));
    *p = 0;
    return fails;
}
