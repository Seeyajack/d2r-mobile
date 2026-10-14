// 鸿蒙侧**出图的地基**：ArkUI 的 `XComponent` 给出的 `OHNativeWindow` → `VkSurfaceKHR`。
// 对标上游苹果侧的 `front_metal_layer`（拿 CAMetalLayer）。见 dm/ohos/图形层实施方案-Vulkan.md 阶段 4。
// The foundation of presenting on HarmonyOS: the OHNativeWindow from an ArkUI XComponent becomes a
// VkSurfaceKHR -- the counterpart of front_metal_layer on the Apple side.
//
// 为什么窗口不由这里创建：窗口是 ArkUI 的（`XComponent`），生命周期由系统管；本模块只**接住**
// 回调递过来的 `OHNativeWindow*`，并在它有效期间建 surface。窗口销毁后必须清空——系统持有引用。
// The window belongs to ArkUI; this module only catches the OHNativeWindow* handed to the callbacks.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// 由 XComponent 的生命周期回调填（napi_init.c 里注册回调）
void   dm_ohos_surface_set_window(void *window, uint32_t w, uint32_t h);
void   dm_ohos_surface_clear_window(void);
void  *dm_ohos_surface_window(void);
int    dm_ohos_surface_size(uint32_t *w, uint32_t *h);

// 用共享上下文的实例建 VkSurfaceKHR。0 = 成功；失败原因写进 err。
int    dm_ohos_surface_create(VkSurfaceKHR *out, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
