// 见 nvn_bc2astc.h。BC 解码复用 nvn_bcdec（bcdec，MIT/公有领域）。
// See nvn_bc2astc.h. BC decoding reuses nvn_bcdec (bcdec, MIT / public domain).
//
// ASTC 块格式依据：Khronos Data Format Specification 1.3 §23（块布局 / 端点模式 / 权重），
// 字段位置与量化表逐项对照过 Google astc-codec 的解码器（Apache-2.0，只作核对，代码未拷贝）。
// ASTC block format per Khronos Data Format Specification 1.3 §23 (block layout / endpoint modes /
// weights); field positions and quantization tables were cross-checked against Google's astc-codec
// decoder (Apache-2.0, used for verification only -- no code copied).
//
// 编码器只做两种固定配置（够用且好验证，质量与代码量的折中）：
//   A) CEM8（LDR RGB 直存，6×8=48 位）+ 权重范围 R=7（3 位 ×16=48 位）—— 不透明块
//   B) CEM12（LDR RGBA 直存，8×8=64 位）+ 权重范围 R=3（2 位 ×16=32 位）—— 带 alpha 的块
// 端点取块内每通道 min/max：通道和 s1 ≥ s0 恒成立，解码器那条"交换端点 + BlueContract"的
// 重构路径不会触发（否则颜色会被改写）。
// Two fixed configurations only: A) CEM8 + R=7 for opaque blocks, B) CEM12 + R=3 for blocks with
// alpha. Endpoints are per-channel min/max, which guarantees s1 >= s0 so the decoder's
// swap + BlueContract path never triggers.
#include "nvn_bc2astc.h"
#include "nvn_bcdec.h"
#include <string.h>

// ── 位读写（ASTC 块按小端位序，bit 0 = 第 0 字节最低位）──
// Bit access: little-endian bit order, bit 0 = LSB of byte 0.
static void put_bits(uint8_t *b, int pos, int n, uint32_t v) {
    for (int i = 0; i < n; i++) {
        int p = pos + i;
        if ((v >> i) & 1u) b[p >> 3] |= (uint8_t)(1u << (p & 7));
        else               b[p >> 3] &= (uint8_t)~(1u << (p & 7));
    }
}
static uint32_t get_bits(const uint8_t *b, int pos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int p = pos + i;
        v |= (uint32_t)((b[p >> 3] >> (p & 7)) & 1u) << i;
    }
    return v;
}

// ── 权重反量化：把 n 位权重位复制到 6 位，>32 再加一（规范 C.2.17）──
// Weight unquantization: bit-replicate the n-bit index to 6 bits, then add one if > 32 (spec C.2.17).
static int weight_unquant(int idx, int nbits) {
    int u = idx, nb = nbits;
    while (nb < 6) {
        int up = (6 - nb < nbits) ? 6 - nb : nbits;
        int down = nbits - up;
        u = (u << up) | (idx >> down);
        nb += up;
    }
    return u > 32 ? u + 1 : u;
}
// 反量化表按需生成（R=3 → 4 档、R=7 → 8 档）
static int weight_nbits(int range) {
    switch (range) {
        case 1: return 1;
        case 3: return 2;
        case 7: return 3;
        case 15: return 4;
        default: return 5;
    }
}

// ── 一块 BC → 4×4 RGBA8（16 字节行距）──
// 先把 BC 解成它自己的未压缩格式（BC4/5 是 R8/RG8），再按"RGBX、a=1"扩成 RGBA8——
// 与 nvn_fmt_mtl.c 里 RGBA 回退路径的扩法一致，两条路对同一张贴图给出一致的采样值。
static int bc_block_rgba8(uint32_t fmt, const uint8_t *src, uint8_t *out) {
    uint32_t pf; int bpp;
    if (!nvn_bc_unpacked(fmt, &pf, &bpp)) return -1;
    uint8_t tmp[4 * 4 * 8];
    if (nvn_bc_decode(fmt, src, 64, 4, 4, tmp, (size_t)4 * bpp) != 0) return -1;
    if (bpp == 4) { memcpy(out, tmp, 64); return 0; }        // RGBA8 直接是结果
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        const uint8_t *s = tmp + (y * 4 + x) * bpp;
        uint8_t *d = out + (y * 4 + x) * 4;
        d[0] = s[0]; d[1] = (bpp >= 2) ? s[1] : 0; d[2] = 0; d[3] = 255;
    }
    return 0;
}

// ── 用编码器的重建公式算误差（与解码器逐位一致，避免"编码器按理想值选、解码器按量化值出"的偏差）──
// 规范 C.2.19：c0=(p0<<8)|p0、c=(c0*(64-w)+c1*w+32)/64、q=(c*255+32767)/65536
static int recon8(int p0, int p1, int w) {
    int c0 = (p0 << 8) | p0, c1 = (p1 << 8) | p1;
    int c = (c0 * (64 - w) + c1 * w + 32) / 64;
    return ((c * 255) + 32767) / 65536;
}

// ── 一块 4×4 RGBA8 → ASTC 4x4 ──
static void astc_encode_block(const uint8_t *px, uint8_t *blk) {
    memset(blk, 0, 16);
    int has_alpha = 0;
    for (int i = 0; i < 16; i++) if (px[i * 4 + 3] != 255) { has_alpha = 1; break; }
    const int cem  = has_alpha ? 12 : 8;         // CEM12 = RGBA 直存，CEM8 = RGB 直存（a 恒 255）
    const int rng  = has_alpha ? 3 : 7;          // 权重范围：3 → 2 位，7 → 3 位
    const int nch  = has_alpha ? 4 : 3;
    const int wbits = weight_nbits(rng);
    const int wtotal = 16 * wbits;

    // 端点：每通道 min/max
    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < nch; c++) {
            int v = px[i * 4 + c];
            if (v < lo[c]) lo[c] = v;
            if (v > hi[c]) hi[c] = v;
        }

    // 权重：把每像素投影到 (hi-lo) 这条线上（最小二乘），再量化到最近的档位
    int d[4], dd = 0;
    for (int c = 0; c < nch; c++) { d[c] = hi[c] - lo[c]; dd += d[c] * d[c]; }
    int nlev = 1 << wbits;
    int wq[16];
    for (int i = 0; i < 16; i++) {
        int num = 0;
        for (int c = 0; c < nch; c++) num += (px[i * 4 + c] - lo[c]) * d[c];
        // t ∈ [0,1] 用定点：t64 = 64 * num / dd
        int t64 = dd ? (int)(((long long)num * 64 + dd / 2) / dd) : 0;
        if (t64 < 0) t64 = 0;
        if (t64 > 64) t64 = 64;
        int best = 0, bestd = 1 << 30;
        for (int k = 0; k < nlev; k++) {
            int u = weight_unquant(k, wbits);
            int diff = u - t64;
            if (diff < 0) diff = -diff;
            if (diff < bestd) { bestd = diff; best = k; }
        }
        wq[i] = best;
    }

    // 块头：mode 位（B4_A2 族）、分区数 1、无 dual plane、CEM
    // 模式判定（astc-codec 的 DecodeBlockMode）：bits[1:0]!=0 且 bits[3:2]==0 ⇒ B4_A2
    //   width  = bits[8:7] + 4 → 4  ⇒ bits[8:7] = 0
    //   height = bits[6:5] + 2 → 4  ⇒ bits[6:5] = 2
    //   权重范围索引 = (bit9<<3) | (bit4 | (bits[1:0]<<1))，表 {-1,-1,1,2,3,4,5,7,...}
    //     R=3 → 索引 4 ⇒ bits[1:0]=2、bit4=0；R=7 → 索引 7 ⇒ bits[1:0]=3、bit4=1
    put_bits(blk, 0, 2, (rng == 7) ? 3u : 2u);
    put_bits(blk, 2, 2, 0);                       // bits[3:2] = 0
    put_bits(blk, 4, 1, (rng == 7) ? 1u : 0u);    // bit 4
    put_bits(blk, 5, 2, 2);                       // height = 4
    put_bits(blk, 7, 2, 0);                       // width = 4
    put_bits(blk, 9, 1, 0);                       // 权重范围高位
    put_bits(blk, 10, 1, 0);                      // 无 dual plane
    put_bits(blk, 11, 2, 0);                      // 分区数 = 1
    put_bits(blk, 13, 4, (uint32_t)cem);

    // 端点：按通道交错 R0,R1,G0,G1,B0,B1[,A0,A1]，各 8 位，从 bit 17 起
    int pos = 17;
    for (int c = 0; c < nch; c++) {
        put_bits(blk, pos, 8, (uint32_t)lo[c]); pos += 8;
        put_bits(blk, pos, 8, (uint32_t)hi[c]); pos += 8;
    }
    // 权重：从块顶（bit 128 - wtotal）起，逐像素 LSB 优先
    int wpos = 128 - wtotal;
    for (int i = 0; i < 16; i++) {
        put_bits(blk, wpos, wbits, (uint32_t)wq[i]);
        wpos += wbits;
    }
}

int nvn_bc2astc_ok(uint32_t f) {
    uint32_t pf; int bpp;
    if (!nvn_bc_unpacked(f, &pf, &bpp)) return 0;
    if (f == 0x4f || f == 0x50) return 0;          // BC6H 是 HDR 半浮点，不走 LDR 的 ASTC 4x4
    return 1;
}
int nvn_bc2astc_bpb(uint32_t f) { return nvn_bc2astc_ok(f) ? 16 : 0; }

int nvn_bc2astc_block(uint32_t f, const uint8_t *src, uint8_t *dst) {
    if (!nvn_bc2astc_ok(f)) return -1;
    uint8_t rgba[64];
    if (bc_block_rgba8(f, src, rgba) != 0) return -1;
    astc_encode_block(rgba, dst);
    return 0;
}

int nvn_bc2astc_region(uint32_t f, const uint8_t *src, size_t src_pitch,
                       uint32_t bx, uint32_t by, uint8_t *dst, size_t dst_pitch) {
    if (!nvn_bc2astc_ok(f)) return -1;
    int bb = (f == 0x41 || f == 0x42 || f == 0x45 || f == 0x46 || f == 0x49 || f == 0x4a) ? 8 : 16;
    for (uint32_t y = 0; y < by; y++)
        for (uint32_t x = 0; x < bx; x++) {
            if (nvn_bc2astc_block(f, src + (size_t)y * src_pitch + (size_t)x * bb,
                                  dst + (size_t)y * dst_pitch + (size_t)x * 16) != 0)
                return -1;
        }
    return 0;
}

// ── 自测解码器：只认本编码器产出的两种配置 ──
int nvn_astc4x4_decode(const uint8_t *blk, uint8_t *dst, size_t dpitch) {
    if (get_bits(blk, 2, 9) == 0x1FC) return -1;               // void extent：不是本编码器产出的
    if (get_bits(blk, 11, 2) != 0) return -1;                  // 只支持 1 分区
    if (get_bits(blk, 10, 1) != 0) return -1;                  // 只支持单平面
    int cem = (int)get_bits(blk, 13, 4);
    if (cem != 8 && cem != 12) return -1;
    // 权重范围索引 → 只接受 3 与 7
    int r = (int)get_bits(blk, 4, 1) | ((int)get_bits(blk, 0, 2) << 1);
    int h = (int)get_bits(blk, 9, 1);
    static const int kRange[16] = {-1, -1, 1, 2, 3, 4, 5, 7, -1, -1, 9, 11, 15, 19, 23, 31};
    int rng = kRange[(h << 3) | r];
    if (rng != 3 && rng != 7) return -1;
    int wbits = weight_nbits(rng), wtotal = 16 * wbits;
    int nch = (cem == 12) ? 4 : 3;

    int lo[4] = {0, 0, 0, 255}, hi[4] = {0, 0, 0, 255};
    int pos = 17;
    for (int c = 0; c < nch; c++) {
        lo[c] = (int)get_bits(blk, pos, 8); pos += 8;
        hi[c] = (int)get_bits(blk, pos, 8); pos += 8;
    }
    // s1 < s0 时解码器要交换端点并做 BlueContract —— 本编码器用 min/max 保证不会发生；
    // 真遇到就按规范做（先交换端点，再对结果的 R/G 做 (R+B)/2、(G+B)/2），免得自测掩盖问题。
    int s0 = lo[0] + lo[1] + lo[2], s1 = hi[0] + hi[1] + hi[2];
    int swapped = s1 < s0;
    int wpos = 128 - wtotal;
    for (int i = 0; i < 16; i++) {
        int k = (int)get_bits(blk, wpos, wbits);
        wpos += wbits;
        int w = weight_unquant(k, wbits);
        int v[4] = {255, 255, 255, 255};
        for (int c = 0; c < nch; c++) {
            int p0 = lo[c], p1 = hi[c];
            if (swapped) { int t = p0; p0 = p1; p1 = t; }
            v[c] = recon8(p0, p1, w);
        }
        if (swapped) { v[0] = (v[0] + v[2]) >> 1; v[1] = (v[1] + v[2]) >> 1; }
        for (int c = 0; c < 4; c++)
            dst[(i / 4) * dpitch + (i % 4) * 4 + c] = (uint8_t)v[c];
    }
    return 0;
}
