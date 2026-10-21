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

// 多线程转码只在 POSIX 上开（鸿蒙/安卓/iOS 都有 pthread；Windows 侧走单线程即可）。
// Multi-threaded transcoding is enabled on POSIX only (HarmonyOS/Android/iOS all have pthreads).
#if defined(__unix__) || defined(__APPLE__) || defined(__OHOS__) || defined(__linux__)
#include <pthread.h>
#include <unistd.h>
#define DM_HAVE_PTHREADS 1
#endif

// ── 位写入：按**字**操作（两个 64 位字 = 128 位块）──
// 原先逐位读改写（每块 113 个 bit，每个 bit 一次分支 + 字节 RMW），实测是转码的热点之一；
// 改成整字或运算后每块只需十几次移位。字节序按小端拼（ARM/x86 都是小端，最后按字节落盘）。
// Word-level bit packing: the old code did 113 branchy byte read-modify-writes per block; this does a
// dozen shifts. Bytes are emitted little-endian explicitly, so no endianness assumption leaks out.
static void put64(uint64_t *w, int pos, int n, uint32_t v) {
    uint64_t mask = ((uint64_t)1 << n) - 1;
    uint64_t val = (uint64_t)v & mask;
    if (pos < 64) {
        w[0] |= val << pos;
        int room = 64 - pos;
        if (n > room) w[1] |= val >> room;      // 跨字的高位部分
    } else {
        w[1] |= val << (pos - 64);
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
    int has_alpha = 0;
    for (int i = 0; i < 16; i++) if (px[i * 4 + 3] != 255) { has_alpha = 1; break; }
    const int cem  = has_alpha ? 12 : 8;         // CEM12 = RGBA 直存，CEM8 = RGB 直存（a 恒 255）
    const int rng  = has_alpha ? 3 : 7;          // 权重范围：3 → 2 位，7 → 3 位
    const int nch  = has_alpha ? 4 : 3;
    const int wbits = weight_nbits(rng);

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
    const int nlev = 1 << wbits;
    // ⭐ 档位表**每块只算一次**：原先在内层循环里对每个像素的每个候选反复调 weight_unquant，
    // 128 次/块（每次还带一个 while 循环），实测占了转码耗时的绝大部分。预生成后内外层只是查表。
    // The level table is built once per block: calling weight_unquant for every pixel x candidate
    // (128 calls/block, each with a loop) used to dominate the transcode cost.
    int lv[32];
    for (int k = 0; k < nlev; k++) lv[k] = weight_unquant(k, wbits);
    int wq[16];
    for (int i = 0; i < 16; i++) {
        int num = 0;
        for (int c = 0; c < nch; c++) num += (px[i * 4 + c] - lo[c]) * d[c];
        // t ∈ [0,1] 用定点：t64 = round(64 * num / dd)。
        // ⚠ 这里**故意用 32 位除法**：num*64+dd/2 <= 1664 万、dd <= 260100，都在 int 范围内。
        // 原先写成了 long long ⇒ 每像素一次 64 位除法（ARM 上是库调用，30-40 拍）；改成 int 除法是
        // 单条指令、且**结果逐位不变**（试过倒数乘，误差 0.4% 会让个别块的权重档差 1 ⇒ 破坏逐字节判据）。
        int t64 = dd ? (num * 64 + dd / 2) / dd : 0;
        if (t64 < 0) t64 = 0;
        if (t64 > 64) t64 = 64;
        int best = 0, bestd = 1 << 30;
        for (int k = 0; k < nlev; k++) {
            int diff = lv[k] - t64;
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
    uint64_t w[2] = { 0, 0 };
    put64(w, 0, 2, (rng == 7) ? 3u : 2u);
    put64(w, 2, 2, 0);                       // bits[3:2] = 0
    put64(w, 4, 1, (rng == 7) ? 1u : 0u);    // bit 4
    put64(w, 5, 2, 2);                       // height = 4
    put64(w, 7, 2, 0);                       // width = 4
    put64(w, 9, 1, 0);                       // 权重范围高位
    put64(w, 10, 1, 0);                      // 无 dual plane
    put64(w, 11, 2, 0);                      // 分区数 = 1
    put64(w, 13, 4, (uint32_t)cem);

    // 端点：按通道交错 R0,R1,G0,G1,B0,B1[,A0,A1]，各 8 位，从 bit 17 起
    int pos = 17;
    for (int c = 0; c < nch; c++) {
        put64(w, pos, 8, (uint32_t)lo[c]); pos += 8;
        put64(w, pos, 8, (uint32_t)hi[c]); pos += 8;
    }
    // 权重：**从块顶往下填**——第 0 个纹素的权重 LSB 在 bit(128-wbits)，第 15 个在最低那段。
    // ⚠ 这里写反过一次：原先从 bit(128-wtotal) 往上填（第 0 个在最低），本地解码器照着同一套写，
    // 于是离线自测全绿；真机 3.4-E 才发现 GPU 读出来是**像素顺序整体反转**——两色棋盘块因为
    // 180° 旋转不变而"侥幸"通过，只有渐变块（块号 2,5,8,…）露馅（20/60 块错、最大差 57）。
    // 教训：编码器与自家解码器同源，**离线自洽不等于对**；必须有第三方（GPU）当判据。
    // Weights are filled from the TOP of the block down: texel 0's weight starts at bit (128-wbits).
    // We had it the other way round once; the local decoder mirrored the same mistake so offline tests
    // passed, and only the GPU (3.4-E) exposed the reversed texel order.
    int wpos = 128 - wbits;
    for (int i = 0; i < 16; i++) {
        put64(w, wpos, wbits, (uint32_t)wq[i]);
        wpos -= wbits;
    }
    for (int i = 0; i < 8; i++) {
        blk[i]     = (uint8_t)(w[0] >> (8 * i));
        blk[8 + i] = (uint8_t)(w[1] >> (8 * i));
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

// 一块 BC 多少字节：BC1/BC4 是 8，其余（BC2/3/5/6H/7）是 16。
// 与 nvn_bcdec.c 顶部那张格式分组保持一致（改一处要改两处）。
static int bc_bpb_bytes(uint32_t f) {
    switch (f) {
    case 0x41: case 0x42: case 0x45: case 0x46: case 0x49: case 0x4a: return 8;
    default: return 16;
    }
}

int nvn_bc2astc_region(uint32_t f, const uint8_t *src, size_t src_pitch,
                       uint32_t bx, uint32_t by, uint8_t *dst, size_t dst_pitch) {
    if (!nvn_bc2astc_ok(f)) return -1;
    int bb = bc_bpb_bytes(f);
    for (uint32_t y = 0; y < by; y++)
        for (uint32_t x = 0; x < bx; x++) {
            if (nvn_bc2astc_block(f, src + (size_t)y * src_pitch + (size_t)x * bb,
                                  dst + (size_t)y * dst_pitch + (size_t)x * 16) != 0)
                return -1;
        }
    return 0;
}

// ── 多线程：按**块行**切分（每块独立、行间无依赖，所以切分不影响结果）──
// 判据：与单线程**逐字节相同**（3.4-E 的判据⑦）。块行是最自然的粒度：不需要锁、不需要原子量。
// Split by block rows: blocks are independent and rows share nothing, so the split cannot change
// the result. The judge is byte-identical output vs the single-threaded path (3.4-E judge 7).
int nvn_bc2astc_cpu_count(void) {
#if defined(DM_HAVE_PTHREADS)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 0;
#else
    return 0;
#endif
}

#if defined(DM_HAVE_PTHREADS)
typedef struct {
    uint32_t f, bx, y0, y1;
    const uint8_t *src; size_t sp;
    uint8_t *dst; size_t dp;
} bc2astc_job;

static void *bc2astc_worker(void *p) {
    bc2astc_job *j = (bc2astc_job *)p;
    int bb = bc_bpb_bytes(j->f);
    for (uint32_t y = j->y0; y < j->y1; y++)
        for (uint32_t x = 0; x < j->bx; x++)
            nvn_bc2astc_block(j->f, j->src + (size_t)y * j->sp + (size_t)x * bb,
                              j->dst + (size_t)y * j->dp + (size_t)x * 16);
    return NULL;
}
#endif

int nvn_bc2astc_region_mt(uint32_t f, const uint8_t *src, size_t src_pitch,
                          uint32_t bx, uint32_t by, uint8_t *dst, size_t dst_pitch,
                          int nthreads) {
    if (!nvn_bc2astc_ok(f)) return -1;
#if defined(DM_HAVE_PTHREADS)
    if (nthreads > (int)by) nthreads = (int)by;      // 行数不够就别开那么多线程
    if (nthreads > 1 && by > 0) {
        pthread_t th[16];
        bc2astc_job job[16];
        int n = nthreads > 16 ? 16 : nthreads;
        uint32_t per = by / (uint32_t)n, rem = by % (uint32_t)n, y = 0;
        int started = 0;
        for (int i = 0; i < n; i++) {
            uint32_t cnt = per + (i < (int)rem ? 1u : 0u);
            job[i] = (bc2astc_job){ f, bx, y, y + cnt, src, src_pitch, dst, dst_pitch };
            y += cnt;
            // 最后一段留给本线程做，省一次建线程/等待
            if (i < n - 1) {
                if (pthread_create(&th[i], NULL, bc2astc_worker, &job[i]) == 0) started++;
                else { bc2astc_worker(&job[i]); th[i] = 0; }
            }
        }
        bc2astc_worker(&job[n - 1]);
        for (int i = 0; i < n - 1; i++) if (th[i]) pthread_join(th[i], NULL);
        (void)started;
        return 0;
    }
#else
    (void)nthreads;
#endif
    return nvn_bc2astc_region(f, src, src_pitch, bx, by, dst, dst_pitch);
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
    int wbits = weight_nbits(rng);
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
    int wpos = 128 - wbits;          // 与编码器同一条约定：第 0 个纹素的权重在块顶
    for (int i = 0; i < 16; i++) {
        int k = (int)get_bits(blk, wpos, wbits);
        wpos -= wbits;
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
