// 图形层能力探测的实现。报告是给人在手机上看/发给 AI 分析的，所以宁可多写一行缘由，不要只丢数字。
// Implementation of the graphics capability probe. The report is meant to be read on the phone (or fed
// to an AI), so a line of "why we need this" beats a bare number.
#include "vk_probe.h"

#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 报告写入

typedef struct { char *p; char *end; } out_t;

static void ap(out_t *o, const char *fmt, ...) {
    if (o->p >= o->end) return;
    va_list a;
    va_start(a, fmt);
    int n = vsnprintf(o->p, (size_t)(o->end - o->p) + 1, fmt, a);
    va_end(a);
    if (n < 0) { o->p = o->end; return; }
    o->p += n;
    if (o->p > o->end) o->p = o->end;
}

static int has_ext(const VkExtensionProperties *list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(list[i].extensionName, name)) return 1;
    return 0;
}

typedef struct { const char *name; const char *why; } want_t;

static void ap_ext_table(out_t *o, const VkExtensionProperties *list, uint32_t n, const want_t *w, int nw) {
    for (int i = 0; i < nw; i++)
        ap(o, "  %-44s %s   %s\n", w[i].name, has_ext(list, n, w[i].name) ? "有" : "无", w[i].why);
}

static const char *dev_type_name(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "集成显卡";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "独立显卡";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "虚拟显卡";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU 软件实现";
        default:                                     return "其它";
    }
}

// 一行一个格式：能不能当采样贴图（游戏要采样它）、能不能当渲染目标（游戏要往它上面画）
static void ap_format(out_t *o, VkPhysicalDevice pd, VkFormat f, const char *label, const char *why) {
    VkFormatProperties fp;
    memset(&fp, 0, sizeof fp);
    vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    int samp = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    int colr = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    ap(o, "  %-30s 采样%s 渲染%s   %s\n", label, samp ? "有" : "无", colr ? "有" : "无", why);
}

// 实例扩展（与出图有关）
static const want_t WANT_INSTANCE_EXT[] = {
    { "VK_KHR_surface", "出图的地基：VkSurfaceKHR 要有它" },
    { "VK_OHOS_surface", "鸿蒙自己的 surface 扩展：把 XComponent 的 OHNativeWindow 接进来（第二阶段用）" },
    { "VK_KHR_get_physical_device_properties2", "Vulkan 1.0 设备上查能力要用它（1.1+ 已内置）" },
};

// 设备扩展（按"移植能不能做"的风险从高到低排）
static const want_t WANT_DEVICE_EXT[] = {
    { "VK_EXT_external_memory_host", "把游戏自己分配的内存直接当显存用（零拷贝）。**没有它也有退路**：见报告末尾" },
    { "VK_KHR_buffer_device_address", "着色器里要用缓冲的设备指针（反射里的 SBUFP）。1.2 起是核心" },
    { "VK_KHR_timeline_semaphore",    "FenceSync / SyncWait 的语义（记栅栏值、等到某个值才放行）。1.2 起是核心" },
    { "VK_KHR_synchronization2",      "现代同步 API，屏障写法更省事。1.3 起是核心" },
    { "VK_KHR_dynamic_rendering",     "不用预先建渲染通道对象；Metal 那边是随时开关渲染编码器。1.3 起是核心" },
    { "VK_EXT_load_store_op_none",    "通道的附件可以不读入/不写回——上游 TBDR 优化的核心手段之一" },
    { "VK_EXT_texture_compression_astc_ldr", "ASTC 压缩贴图：移动 GPU 的主力格式" },
    { "VK_EXT_robustness2",           "越界读被钳住而不是崩（游戏代码可能有越界访问）" },
    { "VK_KHR_image_format_list",     "一个贴图可以有多种兼容格式" },
    { "VK_EXT_host_image_copy",       "主机侧直接拷贴图（省一次中转）" },
    { "VK_KHR_dynamic_rendering_local_read", "动态渲染下的「可读 tile」（上游有专门的可读 tile 着色器版本）" },
    { "VK_EXT_queue_family_foreign",  "跨队列族共享（若图形/计算队列是分开的）" },
};

int dm_vk_probe_run(char *out, size_t cap) {
    if (!out || cap < 256) return -1;
    out_t o = { out, out + cap - 2 };
    *out = 0;

    ap(&o, "=== 鸿蒙 Vulkan 能力探测 ===\n");

    // ---- 加载器版本 ----
    PFN_vkEnumerateInstanceVersion pEnumVer =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(NULL, "vkEnumerateInstanceVersion");
    uint32_t lv = VK_API_VERSION_1_0;
    if (pEnumVer) { VkResult r = pEnumVer(&lv); if (r != VK_SUCCESS) lv = VK_API_VERSION_1_0; }
    ap(&o, "系统 Vulkan 加载器支持到: %u.%u.%u\n",
       VK_API_VERSION_MAJOR(lv), VK_API_VERSION_MINOR(lv), VK_API_VERSION_PATCH(lv));

    // ---- 实例级扩展 ----
    uint32_t nie = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nie, NULL);
    VkExtensionProperties *ie = nie ? (VkExtensionProperties *)malloc(nie * sizeof *ie) : NULL;
    if (ie && vkEnumerateInstanceExtensionProperties(NULL, &nie, ie) != VK_SUCCESS) nie = 0;
    ap(&o, "\n---- 实例级扩展（共 %u 个）----\n", nie);
    ap_ext_table(&o, ie, nie, WANT_INSTANCE_EXT, (int)(sizeof WANT_INSTANCE_EXT / sizeof *WANT_INSTANCE_EXT));

    // ---- 建实例 ----
    uint32_t req = lv;
    if (VK_API_VERSION_MAJOR(req) != 1 || VK_API_VERSION_MINOR(req) > 3) req = VK_API_VERSION_1_1;
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "dm-vkprobe";
    ai.apiVersion = req;
    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult rc = vkCreateInstance(&ci, NULL, &inst);
    if (rc != VK_SUCCESS) { ai.apiVersion = VK_API_VERSION_1_0; rc = vkCreateInstance(&ci, NULL, &inst); }
    ap(&o, "\nvkCreateInstance: %s（请求 %u.%u）\n", rc == VK_SUCCESS ? "成功" : "失败",
       VK_API_VERSION_MAJOR(ai.apiVersion), VK_API_VERSION_MINOR(ai.apiVersion));
    if (rc != VK_SUCCESS) {
        ap(&o, "🔴 连 Vulkan 实例都建不起来（VkResult=%d）——这条路到此为止，要重新评估图形层方案\n", (int)rc);
        free(ie);
        return 0;
    }

    // ---- 物理设备 ----
    uint32_t nd = 0;
    vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (nd == 0) { ap(&o, "🔴 没有物理设备\n"); vkDestroyInstance(inst, NULL); free(ie); return 0; }
    VkPhysicalDevice *pds = (VkPhysicalDevice *)malloc(nd * sizeof *pds);
    vkEnumeratePhysicalDevices(inst, &nd, pds);
    ap(&o, "物理设备数: %u\n", nd);

    VkPhysicalDevice pd = pds[0];
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(pd, &pr);
    ap(&o, "\n===== 设备: %s =====\n", pr.deviceName);
    ap(&o, "  类型: %s   设备 Vulkan 版本: %u.%u.%u   驱动版本: %u\n",
       dev_type_name(pr.deviceType),
       VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion),
       VK_API_VERSION_PATCH(pr.apiVersion), pr.driverVersion);

    // ---- 设备扩展 ----
    uint32_t nde = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &nde, NULL);
    VkExtensionProperties *de = nde ? (VkExtensionProperties *)malloc(nde * sizeof *de) : NULL;
    if (de && vkEnumerateDeviceExtensionProperties(pd, NULL, &nde, de) != VK_SUCCESS) nde = 0;
    ap(&o, "\n---- 设备扩展（共 %u 个），按需求列（有/无 + 为什么需要）----\n", nde);
    ap_ext_table(&o, de, nde, WANT_DEVICE_EXT, (int)(sizeof WANT_DEVICE_EXT / sizeof *WANT_DEVICE_EXT));

    // ---- 贴图格式（本探针最关键的一项）----
    // Switch 用的是桌面式 BCn 压缩；移动 GPU 通常只认 ASTC / ETC2。
    // 这决定"贴图要不要在手机上转码"，是图形层最大的单项工作量差异。
    ap(&o, "\n---- 贴图格式（最关键的一项）----\n");
    ap(&o, "  说明：Switch 打包贴图用 BC 压缩；移动 GPU 一般只支持 ASTC / ETC2。\n");
    ap_format(&o, pd, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 (DXT1)", "Switch 最常用的压缩格式");
    ap_format(&o, pd, VK_FORMAT_BC3_UNORM_BLOCK,      "BC3 (DXT5)", "带 alpha 的常用格式");
    ap_format(&o, pd, VK_FORMAT_BC5_UNORM_BLOCK,      "BC5",        "法线贴图常用");
    ap_format(&o, pd, VK_FORMAT_BC7_UNORM_BLOCK,      "BC7",        "高质量格式");
    ap_format(&o, pd, VK_FORMAT_ASTC_4x4_UNORM_BLOCK,  "ASTC 4x4",  "移动端主力压缩格式");
    ap_format(&o, pd, VK_FORMAT_ASTC_8x8_UNORM_BLOCK,  "ASTC 8x8",  "移动端主力压缩格式");
    ap_format(&o, pd, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, "ETC2 RGB", "移动端备选压缩格式");
    ap_format(&o, pd, VK_FORMAT_R8G8B8A8_UNORM, "RGBA8 未压缩", "解压路径的落点 / UI 贴图");
    ap_format(&o, pd, VK_FORMAT_B8G8R8A8_UNORM, "BGRA8 未压缩", "出图交换链常用的顺序");
    ap_format(&o, pd, VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16F", "HDR / 中间渲染目标");
    ap_format(&o, pd, VK_FORMAT_D32_SFLOAT, "D32 深度", "深度附件");
    ap_format(&o, pd, VK_FORMAT_D24_UNORM_S8_UINT, "D24S8 深度模板", "深度模板附件");

    // ---- 内存模型（零拷贝缓冲的前提）----
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int uma = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) uma++;
    ap(&o, "\n---- 内存模型（零拷贝缓冲的前提）----\n");
    ap(&o, "  内存堆 %u 个，内存类型 %u 个；其中「CPU 能写 + 显存本地」的类型: %u 个 %s\n",
       mp.memoryHeapCount, mp.memoryTypeCount, uma,
       uma ? "（统一内存，零拷贝可行）" : "（非统一：零拷贝要绕道，风险点上浮）");
    for (uint32_t i = 0; i < mp.memoryHeapCount && i < 4; i++)
        ap(&o, "    堆 %u: %.1f MiB  %s\n", i, mp.memoryHeaps[i].size / 1048576.0,
           (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "（显存）" : "（系统内存）");

    // ---- 队列族 ----
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties *qs = (VkQueueFamilyProperties *)malloc(nq * sizeof *qs);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs);
    ap(&o, "\n---- 队列族 %u 个 ----\n", nq);
    for (uint32_t i = 0; i < nq && i < 6; i++) {
        const char *what = (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "图形" :
                           (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)  ? "计算" : "传输";
        ap(&o, "  %u: %s  队列数 %u  时间戳有效位 %u\n", i, what, qs[i].queueCount, qs[i].timestampValidBits);
    }
    int has_ts = 0;
    for (uint32_t i = 0; i < nq; i++) if (qs[i].timestampValidBits) has_ts = 1;
    ap(&o, "  时间戳: %s（上游 DM_NVN_PASSGPU 逐通道 GPU 计时要用）\n", has_ts ? "支持" : "不支持");

    // ---- 关键上限 ----
    ap(&o, "\n---- 关键上限（先列几项最可能不够的）----\n");
    ap(&o, "  每阶段存储缓冲槽 %u（Switch 侧习惯 8 个以上）\n", pr.limits.maxPerStageDescriptorStorageBuffers);
    ap(&o, "  每阶段采样贴图槽 %u / 采样器槽 %u\n",
       pr.limits.maxPerStageDescriptorSampledImages, pr.limits.maxPerStageDescriptorSamplers);
    ap(&o, "  存储缓冲单块最大 %u MiB\n", pr.limits.maxStorageBufferRange >> 20);
    ap(&o, "  颜色附件数 %u / 2D 贴图边长 %u / 帧缓冲 %ux%u\n",
       pr.limits.maxColorAttachments, pr.limits.maxImageDimension2D,
       pr.limits.maxFramebufferWidth, pr.limits.maxFramebufferHeight);
    ap(&o, "  顶点属性 %u / 顶点绑定 %u\n",
       pr.limits.maxVertexInputAttributes, pr.limits.maxVertexInputBindings);
    ap(&o, "  计算工作组 %ux%ux%u，每工作组调用数 %u\n",
       pr.limits.maxComputeWorkGroupSize[0], pr.limits.maxComputeWorkGroupSize[1],
       pr.limits.maxComputeWorkGroupSize[2], pr.limits.maxComputeWorkGroupInvocations);
    ap(&o, "  uniform 缓冲偏移对齐 %u / 非一致内存原子粒度 %u\n",
       (unsigned)pr.limits.minUniformBufferOffsetAlignment, (unsigned)pr.limits.nonCoherentAtomSize);

    // ---- 关键特性 ----
    PFN_vkGetPhysicalDeviceFeatures2 pGetF2 =
        (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2");
    if (!pGetF2)
        pGetF2 = (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(inst, "vkGetPhysicalDeviceFeatures2KHR");

    VkPhysicalDeviceFeatures f0;
    memset(&f0, 0, sizeof f0);
    vkGetPhysicalDeviceFeatures(pd, &f0);
    ap(&o, "\n---- 关键特性（1.0 部分）----\n");
    ap(&o, "  越界访问被钳住 robustBufferAccess: %s\n", f0.robustBufferAccess ? "有" : "无");
    ap(&o, "  独立混合 independentBlend: %s（多渲染目标的混合互相独立）\n", f0.independentBlend ? "有" : "无");
    ap(&o, "  片元可用存储缓冲 fragmentStoresAndAtomics: %s\n", f0.fragmentStoresAndAtomics ? "有" : "无");
    ap(&o, "  顶点可用存储缓冲 vertexPipelineStoresAndAtomics: %s\n", f0.vertexPipelineStoresAndAtomics ? "有" : "无");
    ap(&o, "  存储图像无格式写入 shaderStorageImageWriteWithoutFormat: %s\n",
       f0.shaderStorageImageWriteWithoutFormat ? "有" : "无");
    ap(&o, "  采样贴图数组可动态索引: %s\n", f0.shaderSampledImageArrayDynamicIndexing ? "有" : "无");
    ap(&o, "  存储缓冲数组可动态索引: %s\n", f0.shaderStorageBufferArrayDynamicIndexing ? "有" : "无");
    ap(&o, "  各向异性采样 samplerAnisotropy: %s\n", f0.samplerAnisotropy ? "有" : "无");
    ap(&o, "  深度钳制 depthClamp: %s\n", f0.depthClamp ? "有" : "无");
    ap(&o, "  多采样率着色 sampleRateShading: %s\n", f0.sampleRateShading ? "有" : "无");
    ap(&o, "  压缩贴图 BC / ASTC / ETC2: %s / %s / %s\n",
       f0.textureCompressionBC ? "有" : "无",
       f0.textureCompressionASTC_LDR ? "有" : "无",
       f0.textureCompressionETC2 ? "有" : "无");

    if (pGetF2) {
        VkPhysicalDeviceVulkan12Features f12;
        memset(&f12, 0, sizeof f12);
        f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features f13;
        memset(&f13, 0, sizeof f13);
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceVulkan11Features f11;
        memset(&f11, 0, sizeof f11);
        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceFeatures2 f2;
        memset(&f2, 0, sizeof f2);
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f11;
        f11.pNext = &f12;
        f12.pNext = &f13;
        pGetF2(pd, &f2);

        ap(&o, "\n---- 关键特性（1.1 / 1.2 / 1.3，用 features2 查到的）----\n");
        ap(&o, "  1.1 多视图 multiview: %s（VR / 立体出图用；我们用不到也提一句）\n", f11.multiview ? "有" : "无");
        ap(&o, "  1.1 着色器绘制参数 shaderDrawParameters: %s\n", f11.shaderDrawParameters ? "有" : "无");
        ap(&o, "  1.2 时间线信号量 timelineSemaphore: %s  ← FenceSync/SyncWait 要用\n", f12.timelineSemaphore ? "有" : "无");
        ap(&o, "  1.2 缓冲设备地址 bufferDeviceAddress: %s  ← 着色器里的 SBUFP 要用\n", f12.bufferDeviceAddress ? "有" : "无");
        ap(&o, "  1.2 8 位整数运算 shaderInt8: %s\n", f12.shaderInt8 ? "有" : "无");
        ap(&o, "  1.2 描述符索引动态 indexing: %s\n", f12.shaderStorageBufferArrayNonUniformIndexing ? "有" : "无");
        ap(&o, "  1.3 同步2 synchronization2: %s\n", f13.synchronization2 ? "有" : "无");
        ap(&o, "  1.3 动态渲染 dynamicRendering: %s\n", f13.dynamicRendering ? "有" : "无");
        ap(&o, "  1.3 维护4 maintenance4: %s / ASTC HDR: %s\n",
           f13.maintenance4 ? "有" : "无", f13.textureCompressionASTC_HDR ? "有" : "无");
        ap(&o, "  子组大小（受限于 SIMD 宽度）: 需另查 VkPhysicalDeviceSubgroupProperties\n");
    } else {
        ap(&o, "\n（查不到 features2：设备或加载器只到 Vulkan 1.0，1.1+ 的扩展要逐个查）\n");
    }

    ap(&o, "\n===== 报告结束 =====\n");
    ap(&o, "判读要点（详细见 dm/ohos/图形层能力探测方案.md）：\n");
    ap(&o, "  1. 压缩贴图：BC 全无 + ASTC 有 ⇒ 贴图要在手机上转码（上游已有 DM_TEST_NO_BC 解压路径可借）\n");
    ap(&o, "  2. 统一内存类型 > 0 ⇒ 零拷贝缓冲可行；= 0 ⇒ 由宿主自己分配显存再交给游戏\n");
    ap(&o, "  3. 时间线信号量 / 缓冲设备地址 全有 ⇒ FenceSync 与 SBUFP 可直接映射\n");

    free(qs);
    free(de);
    free(pds);
    free(ie);
    vkDestroyInstance(inst, NULL);
    return 0;
}
