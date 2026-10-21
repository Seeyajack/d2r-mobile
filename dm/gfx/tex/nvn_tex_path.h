// 贴图**上传路径的决策**——平台中立（不认 Vulkan / Metal，只认"设备能力"三个布尔）。
// Texture upload path policy -- platform neutral: no Vulkan / Metal types, only capability flags.
//
// 为什么单独拆出来：同一张 BC 贴图在三种设备上有三种最省的做法，而"选哪条"这件事与平台无关，
// 与后端无关，只与"设备认不认 BC / 认不认 ASTC"有关。把它做成纯函数，苹果侧（Metal）将来填自己的
// 能力表就能复用同一套判断，也方便离线单测（不需要 GPU）。
// Why a separate module: the same BC texture has three cheapest routes on three kinds of hardware, and
// "which route" has nothing to do with the platform or the backend -- only with whether the device can
// sample BC / ASTC. Keeping it a pure function lets the Metal side fill in its own capability flags and
// reuse the same decision, and makes it unit-testable offline (no GPU needed).
//
// 三条路的代价（每像素字节，BC1 记 0.5）：
//   BC 直传      0.5（BC1）/ 1（BC3、BC7）  —— 原样上传，零 CPU 成本、零额外显存
//   ASTC 转码    1                          —— CPU 转码一次，显存只有 RGBA8 的 1/4
//   未压缩回落   4（RGBA8）                 —— 一定能跑，但显存与带宽是 ASTC 的 4 倍
// Costs per texel: BC direct 0.5 (BC1) / 1 (BC3, BC7); ASTC 1 (transcode once on the CPU, 1/4 of
// RGBA8); uncompressed 4 (always works, 4x the VRAM and bandwidth of ASTC).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NVN_TEX_PATH_NONE = 0,        // 无路可走（调用方应报错，别静默传垃圾）
    NVN_TEX_PATH_BC_DIRECT = 1,   // 设备原生认这个 BC 格式 ⇒ 原样直传
    NVN_TEX_PATH_ASTC = 2,        // 转成 ASTC 4x4 再传（转码由 nvn_bc2astc 做）
    NVN_TEX_PATH_UNCOMPRESSED = 3,// 解成未压缩再传（BC 一族解成 RGBA8/RG8/R8；本来就没压缩的照旧）
} nvn_tex_path;

// 设备对**这一个格式**的能力（由平台层探测后填；两个都 0 ⇒ 只能走未压缩）
// Per-format device capabilities, filled in by the platform layer; both zero means uncompressed only.
typedef struct {
    int bc;        // 该格式的 BC 压缩图像能否被采样（Vulkan：optimalTilingFeatures 含 SAMPLED_IMAGE）
    int astc4x4;   // 同一色彩空间的 ASTC 4x4 能否被采样
} nvn_tex_caps;

// 选路。nvn_fmt 是 NVN 的格式号（见 nvn_bcdec.c 顶部那张表）。
// 规则（顺序即优先级）：BC 原生 > ASTC 转码 > 未压缩。
// 非 BC 格式（未压缩 / BC6H 这种 HDR）一律 UNCOMPRESSED —— 本模块不管 HDR 转码。
// Pick a route; priority order: native BC > ASTC transcode > uncompressed. Non-BC formats (already
// uncompressed, or HDR like BC6H) always go uncompressed: HDR transcoding is out of scope here.
nvn_tex_path nvn_tex_path_choose(uint32_t nvn_fmt, nvn_tex_caps caps);

// 走这条路之后**每块多少字节**（块 = 4×4 像素）。未压缩不是块格式 ⇒ 返回 0，调用方按 bpp 算。
// Bytes per 4x4 block after taking this route; 0 for the uncompressed path (it is not block based).
int nvn_tex_path_bpb(uint32_t nvn_fmt, nvn_tex_path path);

// 给报告用的名字（中文，直接进真机日志）。
const char *nvn_tex_path_name(nvn_tex_path path);

#ifdef __cplusplus
}
#endif
