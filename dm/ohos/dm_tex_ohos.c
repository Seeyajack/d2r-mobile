// 见 dm_tex_ohos.h。两件事：查设备能力（查一次缓存起来）+ 把 NVN 格式映射成 VkFormat。
// See dm_tex_ohos.h: probe device capabilities once (cached) and map NVN formats to VkFormat.
#include "dm_tex_ohos.h"

#include "vk_ctx_ohos.h"

#include "../gfx/tex/nvn_bcdec.h"

// 上传 + 采样两条路都要它：上传是 vkCmdCopyBufferToImage（TRANSFER_DST），采样是描述符（SAMPLED_IMAGE）。
// 少任何一个，这条格式对我们都等于"不能用"。
// Both are needed: upload copies into the image (TRANSFER_DST), sampling reads it (SAMPLED_IMAGE).
#define FMT_NEED (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

// 能力缓存：格式号不多（十几条），一次线性查找就够，不值得上哈希。
// Capability cache: a dozen formats, linear search is plenty.
static struct { VkFormat f; int ok; } g_cache[32];
static int g_ncache;

static int fmt_ok_uncached(VkFormat f) {
    VkPhysicalDevice pd = dm_ohos_vk_phys();
    if (!pd || f == VK_FORMAT_UNDEFINED) return 0;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    return (p.optimalTilingFeatures & FMT_NEED) == FMT_NEED;
}

int dm_ohos_fmt_ok(VkFormat f) {
    for (int i = 0; i < g_ncache; i++) if (g_cache[i].f == f) return g_cache[i].ok;
    int ok = fmt_ok_uncached(f);
    if (g_ncache < (int)(sizeof g_cache / sizeof g_cache[0])) {
        g_cache[g_ncache].f = f; g_cache[g_ncache].ok = ok; g_ncache++;
    }
    return ok;
}

void dm_ohos_fmt_features(VkFormat f, VkFormatFeatureFlags *optimal, VkFormatFeatureFlags *linear) {
    VkFormatProperties p;
    VkPhysicalDevice pd = dm_ohos_vk_phys();
    if (optimal) *optimal = 0;
    if (linear) *linear = 0;
    if (!pd || f == VK_FORMAT_UNDEFINED) return;
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    if (optimal) *optimal = p.optimalTilingFeatures;
    if (linear) *linear = p.linearTilingFeatures;
}

int dm_ohos_tex_astc_enabled(void) { return dm_ohos_vk_astc_enabled(); }

// ── NVN 格式 → VkFormat ──
// 这张表与 nvn_bcdec.c 顶部的格式号注释一一对应；sRGB 与否决定直传时用哪个变体。
// 注意 0x41 是 RGB_DXT1（alpha 恒 1）⇒ BC1_RGB；0x42 才是带 alpha 的 BC1_RGBA。
// One-to-one with the format list at the top of nvn_bcdec.c; sRGB picks the variant. 0x41 is
// RGB_DXT1 (alpha = 1) so it maps to BC1_RGB, while 0x42 is the alpha-carrying BC1_RGBA.
static VkFormat bc_vkfmt(uint32_t f) {
    switch (f) {
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 0x4a: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 0x4b: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 0x4c: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4d: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 0x4e: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 0x4f: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    default:   return VK_FORMAT_UNDEFINED;
    }
}

// NVN 的 sRGB BC 格式：0x45–0x48（DXT1/3/5 的 sRGB 版）与 0x4e（BC7 sRGB）。
// 转码后必须落到 ASTC 的 sRGB 变体上，否则颜色空间错了——字节一样，格式声明不一样。
// The sRGB BC formats: 0x45-0x48 and 0x4e. After transcoding they must land on the ASTC sRGB
// variant: same bytes, different format declaration, otherwise the colour space is wrong.
static int bc_is_srgb(uint32_t f) {
    return (f >= 0x45 && f <= 0x48) || f == 0x4e;
}

// nvn_bc_unpacked 给的未压缩格式枚举 → VkFormat（解压路径的落点）。
// 枚举号抄自 nvn_bcdec.c 顶部那行 enum（那边是"贴图自己的未压缩格式"，不是 VkFormat）。
static VkFormat unpacked_vkfmt(uint32_t pf) {
    switch (pf) {
    case 10:  return VK_FORMAT_R8_UNORM;
    case 12:  return VK_FORMAT_R8_SNORM;
    case 30:  return VK_FORMAT_R8G8_UNORM;
    case 32:  return VK_FORMAT_R8G8_SNORM;
    case 70:  return VK_FORMAT_R8G8B8A8_UNORM;
    case 71:  return VK_FORMAT_R8G8B8A8_SRGB;
    case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default:  return VK_FORMAT_UNDEFINED;
    }
}

int dm_ohos_tex_plan(uint32_t nvn_fmt, dm_tex_plan *out) {
    if (!out) return -1;
    out->path = NVN_TEX_PATH_NONE;
    out->fmt = VK_FORMAT_UNDEFINED;
    out->bc_native = 0;
    out->astc_ok = 0;

    uint32_t pf = 0; int bpp = 0;
    if (!nvn_bc_unpacked(nvn_fmt, &pf, &bpp)) return -1;   // 不是 BC 一族 ⇒ 不归本模块管

    // 先探能力，再让平台中立的选路拿主意（顺序不能反：caps 是输入）。
    VkFormat bcf = bc_vkfmt(nvn_fmt);
    VkFormat astc = bc_is_srgb(nvn_fmt) ? VK_FORMAT_ASTC_4x4_SRGB_BLOCK
                                        : VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    nvn_tex_caps caps;
    caps.bc = bcf != VK_FORMAT_UNDEFINED ? dm_ohos_fmt_ok(bcf) : 0;
    // ASTC 只在设备创建时启用了 ASTC LDR 扩展的前提下才算数（1.0–1.3 上是设备级扩展）。
    // ASTC only counts when the ASTC LDR device extension was enabled at device creation.
    caps.astc4x4 = dm_ohos_tex_astc_enabled() ? dm_ohos_fmt_ok(astc) : 0;

    out->path = nvn_tex_path_choose(nvn_fmt, caps);
    out->bc_native = caps.bc;
    out->astc_ok = caps.astc4x4;

    switch (out->path) {
    case NVN_TEX_PATH_BC_DIRECT:    out->fmt = bcf; break;
    case NVN_TEX_PATH_ASTC:         out->fmt = astc; break;
    case NVN_TEX_PATH_UNCOMPRESSED: out->fmt = unpacked_vkfmt(pf); break;
    default:                        return -1;
    }
    return out->fmt == VK_FORMAT_UNDEFINED ? -1 : 0;
}
