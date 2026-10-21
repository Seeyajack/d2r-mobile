// BC → ASTC 4x4 转码（给不支持 BC 压缩格式、但支持 ASTC 的 GPU 用；Mali / Kirin 这一族）。
// BC-to-ASTC 4x4 transcoding (for GPUs without BC support but with ASTC; the Mali / Kirin family).
//
// 为什么需要它：现在 BC 贴图是"CPU 解压成 RGBA8 再上传"（见 nvn_bcdec.h）——BC1 是 0.5 字节/像素、
// RGBA8 是 4 字节/像素，**显存与带宽都 ×8**。BotW 这个量级的贴图靠 RGBA8 装不进手机。
// ASTC 4x4 与 BC 同是"4×4 像素一块、每块 16 字节"（BC1 是 8 字节），块线性布局只取决于块网格与
// 每块字节数 ⇒ 转码后**块与块一一对应**，解排布照旧、不用重排。
// Why: BC textures are currently CPU-decoded to RGBA8 and uploaded uncompressed (see nvn_bcdec.h) --
// BC1 is 0.5 bytes/pixel vs RGBA8's 4, i.e. 8x both in VRAM and bandwidth, which a game of BotW's
// texture volume cannot afford on a phone. ASTC 4x4 has the same 4x4-texel, 16-byte block structure as
// BC3/BC7 (8 bytes for BC1), and the block-linear layout depends only on the block grid and bytes per
// block, so transcoding is a one-to-one per-block operation: the de-swizzle stays as it is.
//
// 本模块只处理**线性块序**（已解排布）的数据——排布由 nvn_deswizzle / nvn_tex 负责，与格式无关。
// This module works on linear block order only (already de-swizzled); layout is nvn_deswizzle / nvn_tex's job.
#pragma once

#include <stddef.h>
#include <stdint.h>

// 该 NVN 格式能否转 ASTC（BC1/2/3/4/5/7 的 LDR 子集；BC6H 是 HDR，不在本模块范围）
// Whether this NVN format can be transcoded (the LDR subset of BC1/2/3/4/5/7; BC6H is HDR, out of scope)
int nvn_bc2astc_ok(uint32_t nvn_fmt);

// 该 NVN 格式转码后的每块字节数（ASTC 4x4 恒为 16）；不支持的格式返回 0
// Bytes per block after transcoding (always 16 for ASTC 4x4); 0 if unsupported
int nvn_bc2astc_bpb(uint32_t nvn_fmt);

// 转一块：src 为一块 BC（8 或 16 字节），dst 为 16 字节 ASTC 4x4。返回 0 成功
// Transcode one block: src is one BC block (8 or 16 bytes), dst is a 16-byte ASTC 4x4 block
int nvn_bc2astc_block(uint32_t nvn_fmt, const uint8_t *src, uint8_t *dst);

// 转一片块网格：src 每块行 src_pitch 字节，dst 每块行 dst_pitch 字节（>= 16*blocks_x）
// Transcode a block grid: src_pitch bytes per block row in src, dst_pitch in dst (>= 16*blocks_x)
int nvn_bc2astc_region(uint32_t nvn_fmt, const uint8_t *src, size_t src_pitch,
                       uint32_t blocks_x, uint32_t blocks_y, uint8_t *dst, size_t dst_pitch);

// 同上，但按**块行**多线程切分（每块独立、行间无依赖 ⇒ 切分不可能改变结果）。
// nthreads <= 1 或平台没有 pthread 时自动退化为单线程。**判据：与单线程逐字节相同。**
// 为什么要它：实测单线程 ≈6 MB/s（2048² 一张 0.7 s），运行时全量转码不可行；
// 转码是天然可并行的（逐块独立），多线程是收益最大、风险最小的第一步。
// Same as above but split across block rows (blocks are independent, so the split cannot change the
// result). Falls back to single-threaded when nthreads <= 1 or pthreads are unavailable.
int nvn_bc2astc_region_mt(uint32_t nvn_fmt, const uint8_t *src, size_t src_pitch,
                          uint32_t blocks_x, uint32_t blocks_y, uint8_t *dst, size_t dst_pitch,
                          int nthreads);

// 建议线程数（0 = 读不到；调用方自己决定上限）
int nvn_bc2astc_cpu_count(void);

// 自测用：把一块 ASTC 4x4 解回 4×4 RGBA8（dst 每行 dpitch 字节）。返回 0 成功。
// 只实现本模块编码器会产生的那两种配置（CEM8/CEM12、单分区、1 平面），供单测与误差评估用。
// For self-test: decode one ASTC 4x4 block back to 4x4 RGBA8. Only the two configurations this
// encoder emits are implemented (CEM8/CEM12, single partition, single plane).
int nvn_astc4x4_decode(const uint8_t *blk, uint8_t *dst, size_t dpitch);
