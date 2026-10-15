#!/usr/bin/env python3
"""从 Switch 卡带镜像（XCI）里抽出分区文件（.nca 等）。

为什么自己写：hactool 的 XCI 模式只列出内容、不落盘（实测 1.4.0），而 HFS0 容器本身很简单、
**分区文件不加密**（加密在 NCA 内部，由 hactool 处理），所以这一层用几十行就够。

用法：
    python xci_extract_nso.py <XCI 文件> <输出目录> [匹配子串 ...]

  不带匹配子串 = 只列出分区与文件；带 = 只抽名字里含任一子串的文件（如 .cnmt.nca）。

格式（公开资料，XCI-Explorer / libnx 同款）：
  游戏卡头 0x1000 字节：0x100 签名、0x100 magic "HEAD"、**0x160 根 HFS0 偏移（u64）**；
  根 HFS0 的条目是各分区（update / normal / secure），每个分区自身又是一个 HFS0。
  HFS0：magic、文件数、字符串表大小、保留；条目 = offset(u64) size(u64) name_hash(u32) name_off(u32)；
  条目里的 offset 相对「本 HFS0 数据区起点」。
"""
import os
import struct
import sys


def u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def parse_hfs0(f, base):
    """base = 这个 HFS0 在文件里的绝对偏移。返回 [(名字, 绝对偏移, 大小)]。
    两种条目步长都认：
      普通 0x18：offset(8) size(8) name_hash(4) name_off(4)   —— NSP 里的 HFS0
      哈希 0x40：offset(8) size(8) name_off(4) hash(4) + 0x28 —— **XCI 分区用这个**（每文件带 SHA-256）
    自动识别：两种步长都试，取「每个 name_off 都落在字符串表内、且名字全是可打印 ASCII」的那种。"""
    f.seek(base)
    head = f.read(16)
    if head[:4] != b"HFS0":
        raise ValueError(f"{base:#x} 处不是 HFS0（读到 {head[:4]!r}）")
    n, strsz = u32(head, 4), u32(head, 8)

    def try_stride(stride, noff_at):
        f.seek(base + 16)
        ents = f.read(n * stride)
        f.seek(base + 16 + n * stride)
        names = f.read(strsz)
        data_off = base + 16 + n * stride + strsz
        out = []
        for k in range(n):
            off, size = struct.unpack_from("<QQ", ents, k * stride)
            noff = u32(ents, k * stride + noff_at)
            if noff >= strsz:
                raise ValueError("name_off 越界")
            end = names.find(b"\0", noff)
            if end == -1:
                raise ValueError("名字没有结尾")
            nm = names[noff:end]
            if not nm or any(c < 0x20 or c > 0x7e for c in nm):
                raise ValueError("名字不是可打印 ASCII")
            out.append((nm.decode(), data_off + off, size))
        return out

    last = None
    for stride, noff_at in ((0x40, 0x10), (0x18, 0x14)):
        try:
            return try_stride(stride, noff_at)
        except ValueError as e:
            last = e
    raise last if last else ValueError("两种步长都解不出")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    xci, out_dir, filters = argv[1], argv[2], argv[3:]
    total = os.path.getsize(xci)
    # 扫全镜像里所有 HFS0（分块、带 0x10 重叠），挑**条目名以 .nca 结尾**的那个分区
    #（卡带根 HFS0 的条目格式有变体，但 secure 分区是普通 HFS0 —— hactool 已证实）。
    CH = 1 << 26      # 分区表永远在文件开头（头部之后），只扫前 64 MB
    limit = min(total, CH)
    best = None
    with open(xci, "rb") as f:
        pos = 0
        while pos < limit:
            f.seek(pos)
            buf = f.read(CH + 16)
            k = buf.find(b"HFS0")
            while k != -1:
                try:
                    ents = parse_hfs0(f, pos + k)
                    n_nca = sum(1 for n, _, _ in ents if n.endswith(".nca"))
                    if n_nca and (best is None or n_nca > best[2]):
                        best = (pos + k, ents, n_nca)
                except Exception:
                    pass
                k = buf.find(b"HFS0", k + 1)
            if pos + CH >= limit:
                break
            pos += CH
        if best is None:
            raise ValueError("全镜像里找不到条目为 .nca 的 HFS0 分区")
        off, ents, n_nca = best
        print(f"内容分区 HFS0 在 {off:#x}，{len(ents)} 个文件（其中 {n_nca} 个 .nca）")

        todo = [(f"{name}", o, sz) for name, o, sz in ents]
        print(f"共 {len(todo)} 个文件：")
        picks = [t for t in todo if not filters or any(m in t[0] for m in filters)]
        for name, off, size in todo:
            mark = "抽" if (not filters or any(m in name for m in filters)) else " "
            print(f"  [{mark}] {name}  {size:,} 字节")
        for name, off, size in picks:
            dst = os.path.join(out_dir, name.replace(":/", "_").replace("/", "_").replace(":", "_"))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(off)
            remain = size
            with open(dst, "wb") as w:
                while remain > 0:
                    chunk = f.read(min(remain, 1 << 20))
                    if not chunk:
                        break
                    w.write(chunk)
                    remain -= len(chunk)
            print(f"  → {dst}（{size:,} 字节）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
