#!/usr/bin/env python3
"""BC→ASTC 4x4 转码的**参考实现**（离线验证算法本身，并与 C 版 nvn_bc2astc.c 逐位对齐）。

为什么有它：
  1. 这台机器上没有宿主 C 编译器（无 MinGW / MSVC / Windows SDK；NDK 的 clang 缺 MinGW sysroot，
     也没有 wasm 目标），C 代码只能在应用构建里编译、在真机上跑。算法对不对，先在 Python 里
     用**同一套位域与量化规则**验一遍——PSNR、块字节、边界都能量化。
  2. C 版实现的是同一套算法；设备上把 C 转出来的 16 字节块与这里的期望值逐字节比对，就能把
     "算法错"和"实现错"分开。最后一步（GPU 认不认）只有真机能答。
Why a Python reference: this machine has no host C compiler (no MinGW / MSVC / Windows SDK; the NDK
clang has no MinGW sysroot and no wasm target), so the C code can only be compiled by the app build
and run on the device. This reference validates the algorithm (same bit layout and quantization
rules) offline: PSNR, block bytes, edge cases. The C implementation mirrors it; on the device the
16-byte blocks can be compared byte-for-byte to separate "algorithm wrong" from "implementation wrong".
Spec: Khronos Data Format 1.3 §23, cross-checked against Google astc-codec (Apache-2.0, read only).

用法：python3 tools/astc_ref.py [--dump]
"""
import struct
import sys
import math

# ───────────────────────── BC 解码（只做测试数据要用的 BC1 / BC3）─────────────────────────
# ⚠ 这里的整数公式**逐条抄自 dm/third_party/bcdec/bcdec.h**，不是"数学上等价"的写法：
#   5→8 位扩展 bcdec 用 (v*527+23)>>6（四舍五入），教科书式位复制 (v<<3)|(v>>2) 在 v=3 时给 24、
#   bcdec 给 25；三色插值 bcdec 也带 +61/+1039 的取整。
#   为什么要抄：C 版的转码输入来自 bcdec 解码，若这里用另一套取整，逐字节比对就会假报差异——
#   那时测到的是"两个解码器不一样"，而不是"编码器写得对不对"。这一条踩过一次，写进注释免得复发。
# The integer formulas below are copied verbatim from bcdec.h. Bit-replication and bcdec's rounded
# expansion disagree for some values (v=3: 24 vs 25), and bcdec's interpolation carries rounding terms.
# The C side decodes with bcdec, so the reference must match it bit for bit -- otherwise a byte-exact
# comparison measures the decoders rather than the encoder.
def _comp(c):
    """565 → 三个原始分量（5/6/5 位，尚未扩展）"""
    return (c >> 11) & 0x1F, (c >> 5) & 0x3F, c & 0x1F

def _expand565(c):
    """565 → 888，取整方式与 bcdec 一致（(v*527+23)>>6 / (v*259+33)>>6）"""
    r, g, b = _comp(c)
    return (r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6

def _interp565(c0, c1, which):
    """bcdec 的第 2/3 号颜色（在 5:6:5 空间内插，带取整），直接给 8 位"""
    r0, g0, b0 = _comp(c0)
    r1, g1, b1 = _comp(c1)
    if which == 2:      # 2/3*c0 + 1/3*c1
        return (((2 * r0 + r1) * 351 + 61) >> 7,
                ((2 * g0 + g1) * 2763 + 1039) >> 11,
                ((2 * b0 + b1) * 351 + 61) >> 7)
    if which == 3:      # 1/3*c0 + 2/3*c1
        return (((r0 + 2 * r1) * 351 + 61) >> 7,
                ((g0 + 2 * g1) * 2763 + 1039) >> 11,
                ((b0 + 2 * b1) * 351 + 61) >> 7)
    # 1/2*c0 + 1/2*c1（BC1 的三色模式）
    return (((r0 + r1) * 1053 + 125) >> 8,
            ((g0 + g1) * 4145 + 1019) >> 11,
            ((b0 + b1) * 1053 + 125) >> 8)

def bc1_decode(blk, opaque=False):
    """opaque=True 对应 bcdec 的 onlyOpaqueMode（BC3 的颜色块恒走四色模式）"""
    c0, c1, bits = struct.unpack_from('<HHI', blk, 0)
    p0, p1 = _expand565(c0), _expand565(c1)
    if c0 > c1 or opaque:
        pal = [p0, p1, _interp565(c0, c1, 2), _interp565(c0, c1, 3)]
    else:
        pal = [p0, p1, _interp565(c0, c1, 0), (0, 0, 0)]
    out = []
    for i in range(16):
        idx = (bits >> (2 * i)) & 3
        r, g, b = pal[idx]
        out.append((r, g, b, 255))
    return out

def _bc3_alpha(a0, a1, bits):
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]

def bc3_decode(blk):
    a0, a1 = blk[0], blk[1]
    abits = int.from_bytes(blk[2:8], 'little')
    alpha = _bc3_alpha(a0, a1, abits)
    col = bc1_decode(blk[8:16], opaque=True)   # BC3 的颜色块恒为四色模式（bcdec: onlyOpaqueMode=1）
    return [(c[0], c[1], c[2], alpha[i]) for i, c in enumerate(col)]

# ───────────────────────── ASTC 编码（与 C 版同一算法）─────────────────────────
def weight_unquant(idx, nbits):
    """位复制到 6 位，>32 再加一（规范 C.2.17）"""
    u, nb = idx, nbits
    while nb < 6:
        up = min(nbits, 6 - nb)
        down = nbits - up
        u = (u << up) | (idx >> down)
        nb += up
    return u + 1 if u > 32 else u

def weight_nbits(rng):
    return {1: 1, 3: 2, 7: 3, 15: 4}.get(rng, 5)

def recon8(p0, p1, w):
    c0 = (p0 << 8) | p0
    c1 = (p1 << 8) | p1
    c = (c0 * (64 - w) + c1 * w + 32) // 64
    return (c * 255 + 32767) // 65536

def astc_encode(px):
    """px: 16 个 (r,g,b,a) → 16 字节 ASTC 4x4（CEM8+R7 或 CEM12+R3）"""
    has_alpha = any(p[3] != 255 for p in px)
    cem = 12 if has_alpha else 8
    rng = 3 if has_alpha else 7
    nch = 4 if has_alpha else 3
    wbits = weight_nbits(rng)

    lo = [255] * 4
    hi = [0] * 4
    for p in px:
        for c in range(nch):
            lo[c] = min(lo[c], p[c])
            hi[c] = max(hi[c], p[c])
    d = [hi[c] - lo[c] for c in range(4)]
    dd = sum(d[c] * d[c] for c in range(nch))
    nlev = 1 << wbits
    wq = []
    for p in px:
        num = sum((p[c] - lo[c]) * d[c] for c in range(nch))
        t64 = (num * 64 + dd // 2) // dd if dd else 0
        t64 = max(0, min(64, t64))
        best, bestd = 0, 1 << 30
        for k in range(nlev):
            diff = abs(weight_unquant(k, wbits) - t64)
            if diff < bestd:
                bestd, best = diff, k
        wq.append(best)

    bits = 0
    def put(pos, n, v):
        nonlocal bits
        bits |= (v & ((1 << n) - 1)) << pos
    put(0, 2, 3 if rng == 7 else 2)     # bits[1:0]
    put(2, 2, 0)                        # bits[3:2] = 0 → B4_A2
    put(4, 1, 1 if rng == 7 else 0)     # bit4
    put(5, 2, 2)                        # height = 4
    put(7, 2, 0)                        # width = 4
    put(9, 1, 0)                        # 权重范围高位
    put(10, 1, 0)                       # 无 dual plane
    put(11, 2, 0)                       # 1 分区
    put(13, 4, cem)
    pos = 17
    for c in range(nch):                # 端点按通道交错 R0,R1,G0,G1,...
        put(pos, 8, lo[c]); pos += 8
        put(pos, 8, hi[c]); pos += 8
    # 权重**从块顶往下填**：第 0 个纹素的权重 LSB 在 bit(128-wbits)。
    # 写反过一次：原先从 bit(128-16*wbits) 往上填，本地解码器跟着同一套错，离线自测全绿；
    # 真机 3.4-E 才暴露——GPU 读出来像素顺序整体反转，两色棋盘块因 180° 旋转不变而侥幸通过，
    # 只有渐变块露馅（20/60 块错、最大差 57）。离线自洽 ≠ 对，必须拿 GPU 当判据。
    wpos = 128 - wbits
    for k in wq:
        put(wpos, wbits, k); wpos -= wbits
    return bits.to_bytes(16, 'little')

# ───────────────────────── ASTC 解码（只认上面两种配置，用于自测）─────────────────────────
def astc_decode(blk):
    bits = int.from_bytes(blk, 'little')
    def get(pos, n):
        return (bits >> pos) & ((1 << n) - 1)
    if get(2, 9) == 0x1FC:
        raise ValueError('void extent：不是本编码器的产物')
    if get(11, 2) != 0 or get(10, 1) != 0:
        raise ValueError('只支持单分区单平面')
    cem = get(13, 4)
    if cem not in (8, 12):
        raise ValueError(f'只支持 CEM8/12，拿到 {cem}')
    r = get(4, 1) | (get(0, 2) << 1)
    h = get(9, 1)
    rng = [-1, -1, 1, 2, 3, 4, 5, 7, -1, -1, 9, 11, 15, 19, 23, 31][(h << 3) | r]
    if rng not in (3, 7):
        raise ValueError(f'权重范围 {rng} 不在本解码器范围')
    wbits = weight_nbits(rng)
    nch = 4 if cem == 12 else 3
    lo = [0, 0, 0, 255]
    hi = [0, 0, 0, 255]
    pos = 17
    for c in range(nch):
        lo[c] = get(pos, 8); pos += 8
        hi[c] = get(pos, 8); pos += 8
    swapped = (hi[0] + hi[1] + hi[2]) < (lo[0] + lo[1] + lo[2])
    wpos = 128 - wbits          # 与编码器同一条约定：第 0 个纹素的权重在块顶
    out = []
    for _ in range(16):
        k = get(wpos, wbits); wpos -= wbits
        w = weight_unquant(k, wbits)
        v = [255, 255, 255, 255]
        for c in range(nch):
            p0, p1 = (hi[c], lo[c]) if swapped else (lo[c], hi[c])
            v[c] = recon8(p0, p1, w)
        if swapped:
            v[0] = (v[0] + v[2]) >> 1
            v[1] = (v[1] + v[2]) >> 1
        out.append(tuple(v))
    return out

# ───────────────────────── 判据 ─────────────────────────
def psnr(a, b, chans=(0, 1, 2, 3)):
    se = 0
    n = 0
    for p, q in zip(a, b):
        for c in chans:
            d = p[c] - q[c]
            se += d * d
            n += 1
    if se == 0:
        return 99.0
    mse = se / n
    return 10.0 * math.log10((255.0 * 255.0) / mse)

def mk_bc3(px):
    """把 16 个像素编成一块 BC3（只为造测试数据：alpha 用 8 档、颜色用 BC1 4 色）"""
    # alpha：取 min/max 端点 + 8 档最近索引
    alphas = [p[3] for p in px]
    a0, a1 = max(alphas), min(alphas)
    if a0 > a1:
        pal = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        pal = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
    abits = 0
    for i, p in enumerate(px):
        k = min(range(len(pal)), key=lambda j: abs(pal[j] - p[3]))
        abits |= (k & 7) << (3 * i)
    # 颜色：取对角两端点做 RGB565（c0>c1 ⇒ 4 色模式）
    def q565(p):
        r = min(31, p[0] >> 3); g = min(63, p[1] >> 2); b = min(31, p[2] >> 3)
        return (r << 11) | (g << 5) | b
    lum = lambda p: p[0] * 3 + p[1] * 6 + p[2]
    hi_px = max(px, key=lum)
    lo_px = min(px, key=lum)
    c0, c1 = q565(hi_px), q565(lo_px)
    if c0 <= c1:
        c0, c1 = c1, c0
    p0, p1 = _expand565(c0), _expand565(c1)
    pal_c = [p0, p1,
             tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
             tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    cbits = 0
    for i, p in enumerate(px):
        k = min(range(4), key=lambda j: (pal_c[j][0] - p[0]) ** 2 + (pal_c[j][1] - p[1]) ** 2 + (pal_c[j][2] - p[2]) ** 2)
        cbits |= k << (2 * i)
    return bytes([a0, a1]) + abits.to_bytes(6, 'little') + struct.pack('<HHI', c0, c1, cbits)

def mk_bc1(px):
    def q565(p):
        return ((min(31, p[0] >> 3)) << 11) | ((min(63, p[1] >> 2)) << 5) | min(31, p[2] >> 3)
    lum = lambda p: p[0] * 3 + p[1] * 6 + p[2]
    hi_px, lo_px = max(px, key=lum), min(px, key=lum)
    c0, c1 = q565(hi_px), q565(lo_px)
    if c0 <= c1:
        c0, c1 = c1, c0
    p0, p1 = _expand565(c0), _expand565(c1)
    pal = [p0, p1,
           tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
           tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    cbits = 0
    for i, p in enumerate(px):
        k = min(range(4), key=lambda j: (pal[j][0] - p[0]) ** 2 + (pal[j][1] - p[1]) ** 2 + (pal[j][2] - p[2]) ** 2)
        cbits |= k << (2 * i)
    return struct.pack('<HHI', c0, c1, cbits)

# ───────────────────────── 真机期望表（3.4-E 用）─────────────────────────
# 为什么要把"输入"也写进头文件：设备侧要么自己造一份 BC3（那就得把 mk_bc3 逐位复刻到 C 里，
# 多一处可能不一致的地方），要么直接用这里生成的字节。后者只留一个可能出错的地方——算法本身。
# 输入 40×24：非方形、非 2 的幂，行距/块行算错会立刻暴露。
# The input is emitted too: otherwise the C side would have to re-implement mk_bc3 bit for bit,
# adding another place where the two sides can silently disagree.
HDR_W, HDR_H = 40, 24

def make_bc3_image(w=HDR_W, h=HDR_H):
    """造一张 BC3 贴图（线性块序）。图案刻意做成三点：
       · 每块两种颜色、块内棋盘交错 ⇒ x/y 翻转、行距错、块序错都会被抓到；
       · 每 3 块有 1 块带 alpha，且 alpha 与颜色**同步**（遮罩型）⇒ 编码器两条配置
         （CEM12+R3 / CEM8+R7）都会被走到，而块内仍是"两色"⇒ 单分区编码可做到近乎无损，
         这样真机判据能把阈值收到很紧（见 astc_expect.h 顶部的参考 PSNR）。
       · 不造"颜色棋盘 + 独立 alpha 渐变"的块：单分区 + 2 位权重无法同时表达两条独立方向，
         那种块会掉到 18 dB 左右，属于**已知的结构性限制**（离线自测里量过），不适合拿来当接线判据。
    """
    out = bytearray()
    for by in range(h // 4):
        for bx in range(w // 4):
            kind = (bx + by) % 3
            # 两种颜色**逐通道单调**（第二种比第一种各高 96），且 base 取模 0x7F 保证 +96 不溢出。
            # 为什么强调单调：单分区编码把"块内两色"当成一条线；两色若不单调（某通道反转），
            # 最小二乘投影会落到线中段，权重取到中间档 ⇒ 两种颜色都被压向中间（踩过：PSNR 掉到 17 dB，
            # 原因是 `& 0xFF` 让第二种颜色绕回暗端，与 alpha 变成反相关）。
            base_r = (bx * 23 + by * 31) & 0x7F
            base_g = (by * 17 + bx * 13) & 0x7F
            base_b = (bx * 29 + by * 7) & 0x7F
            lo_c = (32 + base_r, 32 + base_g, 32 + base_b)
            hi_c = (lo_c[0] + 96, lo_c[1] + 96, lo_c[2] + 96)
            px = []
            for y in range(4):
                for x in range(4):
                    if kind == 2:
                        # 连续色调（块内二维渐变、不透明）：走 CEM8+R7，有损但是可预期的损失，
                        # 用来给"真贴图那种连续色"留一个样本（两色块转码是无损的，抓不到量化误差）。
                        px.append((min(255, 40 + x * 12 + bx * 3),
                                   min(255, 40 + y * 12 + by * 3),
                                   min(255, 60 + (x + y) * 9),
                                   255))
                        continue
                    if ((x ^ y) & 1) == 0:            # 暗色那一半
                        r, g, b = lo_c
                        a = 96 if kind == 0 else 255  # kind 0 = 遮罩型 alpha（与颜色同步）
                    else:                              # 亮色那一半
                        r, g, b = hi_c
                        a = 255
                    px.append((r, g, b, a))
            out += mk_bc3(px)
    return bytes(out)

def _c_array(name, data, per_line=16):
    lines = [f'static const uint8_t {name}[{len(data)}] = {{']
    for i in range(0, len(data), per_line):
        chunk = ', '.join(f'0x{v:02X}' for v in data[i:i + per_line])
        lines.append(f'    {chunk},' + (f'   // 块 {i // 16}' if per_line == 16 and len(data) % 16 == 0 else ''))
    lines.append('};')
    return '\n'.join(lines)

def emit_header(path):
    bc = make_bc3_image()
    astc = bytearray()
    ref_all, back_all = [], []
    n_mask = n_two = n_grad = 0
    for i in range(0, len(bc), 16):
        ref = bc3_decode(bc[i:i + 16])
        blk = astc_encode(ref)
        astc += blk
        ref_all += ref
        back_all += astc_decode(blk)
        has_a = any(p[3] != 255 for p in ref)
        # 分类只用于报告文字：渐变块 = 块内像素不止两种颜色
        if has_a:
            n_mask += 1
        elif len(set(ref)) <= 2:
            n_two += 1
        else:
            n_grad += 1
    # 只算 RGB：与设备侧 tex_psnr_rgb 同一口径（alpha 在真贴图里多是遮罩，混进来会掩盖颜色误差）。
    # RGB only, matching the device-side tex_psnr_rgb -- mixing alpha in would mask colour error.
    p = psnr(ref_all, back_all, chans=(0, 1, 2))
    hdr = f'''// 生成文件，**不要手改**：python tools/ohos/astc_ref.py --emit-header
// Generated file -- do not edit by hand; see the command above.
//
// 3.4-E（C3 设备接线）的期望表：左边是"游戏给的 BC3 字节"（线性块序），右边是同一条链在参考实现
// 里的转码结果（ASTC 4x4，每块 16 字节）。设备上 C 版 nvn_bc2astc 的产出与右边**逐字节**比。
// Expected table for 3.4-E: the BC3 source bytes the game would hand us, and the reference
// transcode output (ASTC 4x4, 16 bytes per block) that the C implementation must match byte for byte.
//
// 贴图 {HDR_W}×{HDR_H}（{HDR_W // 4}×{HDR_H // 4} = {len(bc) // 16} 块），三种块交替出现：
//   · 遮罩型 alpha（{n_mask} 块）—— 两色 + alpha 与颜色同步，走 CEM12+R3，转码应无损；
//   · 不透明两色（{n_two} 块）—— 走 CEM8+R7，转码应无损；
//   · 连续色调（{n_grad} 块）—— 块内二维渐变，走 CEM8+R7，**有损**（量化误差在这里才看得到）。
// 参考链自评：PSNR = {p:.2f} dB。设备侧判据 = 不差于参考值 0.5 dB（字节既然已经逐字节相等，
// 理论上应当一致；留 0.5 dB 是给 GPU 解码器的舍入差）。
#pragma once

#include <stdint.h>

#define ASTC_EXPECT_W {HDR_W}
#define ASTC_EXPECT_H {HDR_H}
#define ASTC_EXPECT_BLOCKS {len(bc) // 16}
#define ASTC_EXPECT_BC3_BYTES {len(bc)}
#define ASTC_EXPECT_ASTC_BYTES {len(astc)}
#define ASTC_EXPECT_REF_PSNR {p:.2f}f

// 源：BC3（NVN 0x44 DXT5）线性块序，每块 16 字节
{_c_array('astc_expect_bc3', bc)}
// 期望：转码后的 ASTC 4x4，每块 16 字节
{_c_array('astc_expect_astc', astc)}
'''
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(hdr)
    print(f'✓ 写出 {path}：{len(bc) // 16} 块（遮罩 alpha {n_mask} / 两色 {n_two} / 渐变 {n_grad}）、'
          f'BC3 {len(bc)} 字节 → ASTC {len(astc)} 字节，参考 PSNR {p:.2f} dB')
    return 0

# ───────────────────────── 测试 ─────────────────────────
def cases():
    """造几种典型块：纯色、水平渐变、对角渐变、噪声、带 alpha 的渐变"""
    flat = [(120, 130, 140, 255)] * 16
    grad_h = [((x * 17 + 40) & 0xFF, (x * 11 + 60) & 0xFF, (x * 7 + 80) & 0xFF, 255)
              for y in range(4) for x in range(4)]
    grad_d = [(((x + y) * 32) & 0xFF, ((x + y) * 20 + 30) & 0xFF, 90, 255)
              for y in range(4) for x in range(4)]
    noise = [((x * 73 + y * 151) & 0xFF, (x * 37 + y * 89) & 0xFF, (x * 199 + y * 31) & 0xFF, 255)
             for y in range(4) for x in range(4)]
    alpha_grad = [(200, 100, 50, (x + y) * 18) for y in range(4) for x in range(4)]
    return [('纯色', flat), ('水平渐变', grad_h), ('对角渐变', grad_d), ('噪声', noise), ('带alpha渐变', alpha_grad)]

def main():
    if '--emit-header' in sys.argv:
        i = sys.argv.index('--emit-header')
        out = sys.argv[i + 1] if len(sys.argv) > i + 1 else 'dm/ohos/astc_expect.h'
        return emit_header(out)
    dump = '--dump' in sys.argv
    print('=== BC→ASTC 4x4 参考实现自测（离线）===')
    print(f'{"用例":<12} {"编码配置":<10} {"PSNR(dB)":>9}  {"块字节":<32}')
    ok = True
    for name, px in cases():
        bc = mk_bc3(px)                     # 先用 BC3 编码成"源"（模拟游戏贴图）
        ref = bc3_decode(bc)                # BC 解码结果 = 转码的输入参照
        blk = astc_encode(ref)              # 转码
        back = astc_decode(blk)             # 解回
        p = psnr(ref, back)
        has_a = any(q[3] != 255 for q in ref)
        cfg = 'CEM12+R3' if has_a else 'CEM8+R7'
        print(f'{name:<12} {cfg:<10} {p:9.2f}  {blk.hex()}')
        if p < 28.0:
            ok = False
            print(f'  ⚠ PSNR 低于阈值 28 dB')
        if dump:
            print('  BC3 :', bc.hex())
            print('  源像素:', ref[:4], '...')
            print('  解回  :', back[:4], '...')
    print()
    print('✓ 全部用例 PSNR ≥ 28 dB' if ok else '✗ 有用例不达标')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
