// 出图地基的实现：OHNativeWindow → VkSurfaceKHR。接口与理由见 vk_surface_ohos.h。
// Implementation of the present foundation: OHNativeWindow -> VkSurfaceKHR.
//
// 必须先定义 VK_USE_PLATFORM_OHOS 再包含 vulkan.h —— 否则 vulkan_ohos.h 不会被引进来，
// VkSurfaceCreateInfoOHOS / vkCreateSurfaceOHOS 就都看不到（NDK 的 vulkan.h 里是 #ifdef 守着的）。
#define VK_USE_PLATFORM_OHOS 1
#include "vk_surface_ohos.h"

#include "dm_log.h"
#include "vk_ctx_ohos.h"

#include <stdio.h>
#include <string.h>

static void    *g_window;
static uint32_t g_w, g_h;
static void    *g_xc;      // ArkUI 的 XComponent 实例（帧率接口要用，见头文件里的理由）

void dm_ohos_surface_set_window(void *window, uint32_t w, uint32_t h) {
    g_window = window; g_w = w; g_h = h;
    LOG("出图：XComponent 窗口 %p，%ux%u", window, w, h);
}
void dm_ohos_surface_clear_window(void) {
    g_window = NULL; g_w = g_h = 0;
    LOG("出图：XComponent 窗口已销毁，已清空");
}
void  *dm_ohos_surface_window(void) { return g_window; }
void   dm_ohos_surface_set_xc(void *xc) {
    g_xc = xc;
    LOG("帧率接口：拿到 ArkUI 的 XComponent 实例 %p", xc);
}
void  *dm_ohos_surface_xc(void) { return g_xc; }
int    dm_ohos_surface_size(uint32_t *w, uint32_t *h) {
    if (!g_window || !g_w || !g_h) return -1;
    if (w) *w = g_w;
    if (h) *h = g_h;
    return 0;
}

int dm_ohos_surface_create(VkSurfaceKHR *out, char *err, size_t errcap) {
    *out = VK_NULL_HANDLE;
    if (!g_window) { snprintf(err, errcap, "还没有 XComponent 窗口（surface 回调没到？）"); return -1; }

    // vkCreateSurfaceOHOS 是扩展函数：加载器不一定给静态符号，按实例取函数指针最稳。
    PFN_vkCreateSurfaceOHOS pCreate =
        (PFN_vkCreateSurfaceOHOS)vkGetInstanceProcAddr(dm_ohos_vk_instance(), "vkCreateSurfaceOHOS");
    if (!pCreate) { snprintf(err, errcap, "拿不到 vkCreateSurfaceOHOS（VK_OHOS_surface 不可用？）"); return -1; }

    VkSurfaceCreateInfoOHOS ci = { 0 };
    ci.sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
    ci.window = (OHNativeWindow *)g_window;
    VkResult r = pCreate(dm_ohos_vk_instance(), &ci, NULL, out);
    if (r != VK_SUCCESS) { snprintf(err, errcap, "vkCreateSurfaceOHOS 失败（VkResult=%d）", (int)r); return -1; }
    LOG("出图：VkSurfaceKHR 建好了 %p", (void *)*out);
    return 0;
}
