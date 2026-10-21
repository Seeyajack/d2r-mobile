// 鸿蒙侧贴图路径的**落地**：把平台中立的选路（gfx/tex/nvn_tex_path.h）接到真实设备能力上，
// 并给出上传时要用的 VkFormat。生产路径与真机测试都从这里取答案。
// HarmonyOS side of the texture path decision: wires the platform-neutral policy to the real device
// capabilities and yields the VkFormat to upload with. Both production and the on-device tests ask here.
//
// 为什么"选路"和"设备能力"要分成两层：
//   · 选路（BC 直传 / ASTC 转码 / 未压缩）与平台无关，放 gfx/tex，苹果侧也能用；
//   · 能力探测只有平台知道（Vulkan 查 formatProperties，Metal 查 device.supportsFamily），放这里。
//   这样以后接 Metal 时，换的只是下面这一层的实现，判断逻辑一行都不用改。
// Why two layers: the route choice is platform independent (gfx/tex, reusable on the Metal side) while
// capability probing is inherently platform specific. Porting to Metal later only replaces this layer.
#pragma once

#include <stdint.h>

#include <vulkan/vulkan.h>

#include "../gfx/tex/nvn_tex_path.h"

#ifdef __cplusplus
extern "C" {
#endif

// 一张贴图的落地计划：走哪条路 + 上传时用哪个 VkFormat + 判断依据（报告里要打出来）。
// The plan for one texture: route, VkFormat to upload with, and the evidence behind the decision.
typedef struct {
    nvn_tex_path path;
    VkFormat     fmt;         // 上传/采样用的图像格式（UNCOMPRESSED 时是解码后的未压缩格式）
    int          bc_native;   // 这个格式的 BC 压缩图像本机能否采样（0/1）
    int          astc_ok;     // 同一色彩空间的 ASTC 4x4 能否采样（0/1）
} dm_tex_plan;

// 给出计划。0 = 成功；非 0 = 该 NVN 格式本模块不认识（调用方沿用原有路径）。
// Fills *out; returns 0 on success, non-zero when this NVN format is not ours to decide.
int dm_ohos_tex_plan(uint32_t nvn_fmt, dm_tex_plan *out);

// 单个格式能否"既当上传目标又能被采样"（optimal tiling）。
// 判据取 `SAMPLED_IMAGE | TRANSFER_DST`：上传走 vkCmdCopyBufferToImage，采样走描述符。
// Whether this format can be both an upload target and sampled (optimal tiling).
int dm_ohos_fmt_ok(VkFormat f);

// 原始读数（optimal / linear 两组 feature 位），报告里逐个格式打出来用。
void dm_ohos_fmt_features(VkFormat f, VkFormatFeatureFlags *optimal, VkFormatFeatureFlags *linear);

// 设备创建时到底有没有启用 `VK_EXT_texture_compression_astc_ldr`（Vulkan 1.0–1.3 上不启用就用不了
// ASTC 格式）。报告里必须打这一条：ASTC 图像建不出来时，先看它是 0 还是 1。
// Whether the ASTC LDR device extension was actually enabled at device creation.
int dm_ohos_tex_astc_enabled(void);

#ifdef __cplusplus
}
#endif
