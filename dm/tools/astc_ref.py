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
def _rgb565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    # 按 BC 规范做位复制扩展（5/6/5 → 8）
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))

def bc1_decode(blk):
    c0, c1, bits = struct.unpack_from('<HHI', blk, 0)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1:
        pal = [p0, p1,
               tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    else:
        pal = [p0, p1,
               tuple((a + b) // 2 for a, b in zip(p0, p1)),
               (0, 0, 0)]
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
    col = bc1_decode(blk[8:16])          # BC3 的颜色块就是 BC1（4 色模式恒开）
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
    wpos = 128 - 16 * wbits
    for k in wq:
        put(wpos, wbits, k); wpos += wbits
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
    wpos = 128 - 16 * wbits
    out = []
    for _ in range(16):
        k = get(wpos, wbits); wpos += wbits
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
    p0, p1 = _rgb565(c0), _rgb565(c1)
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
    p0, p1 = _rgb565(c0), _rgb565(c1)
    pal = [p0, p1,
           tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
           tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
    cbits = 0
    for i, p in enumerate(px):
        k = min(range(4), key=lambda j: (pal[j][0] - p[0]) ** 2 + (pal[j][1] - p[1]) ** 2 + (pal[j][2] - p[2]) ** 2)
        cbits |= k << (2 * i)
    return struct.pack('<HHI', c0, c1, cbits)

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
