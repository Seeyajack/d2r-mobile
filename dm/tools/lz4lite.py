#!/usr/bin/env python3
"""LZ4 block 解压（纯 Python，无第三方依赖）——给 NSO 解析与改写器用。

为什么自己写（2026-10-22）：
  `patch_nso.py` / `nso.py` 原本 `import lz4.block`，而本机没装 python-lz4 ⇒ 换游戏/换机器就得先 pip。
  但这条链是"一键移植"工具链的一部分，**必须零依赖**（用户机器上不该为跑一次改写去装包）。
  纯 Python 版本对 ~10 MB 的模块够快（几秒），换来的是"拷贝目录即可运行"。

LZ4 block 格式（只做解压）：序列 = [token][literals][offset:2][match_len 扩展]…
  token 高 4 位 = literal 长度（15 表示后面还有 1 字节累加），低 4 位 = match 长度 - 4（同样有扩展）。
"""


def decompress_block(src: bytes, out_size: int = 0) -> bytes:
    dst = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255:
                    break
        if lit:
            dst += src[i:i + lit]
            i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(dst):
            raise ValueError(f'LZ4 偏移非法：off={off} 已有={len(dst)}')
        mlen = token & 0x0F
        if mlen == 15:
            while True:
                b = src[i]; i += 1; mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(dst) - off
        for k in range(mlen):          # 逐字节拷：LZ4 允许重叠拷贝
            dst.append(dst[start + k])
    if out_size and len(dst) != out_size:
        raise ValueError(f'LZ4 解压长度不符：得到 {len(dst)}，期望 {out_size}')
    return bytes(dst)
