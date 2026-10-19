#!/usr/bin/env python3
"""模块普查（离线）：某个 Switch 模块（NSO）到底导出了什么、导入了什么。

为什么需要（2026-10-19 调研 seedds/nxnative 后的第一刀）：
  同源分支 nxnative 删掉了 136 条 nn::os 的 HLE，理由是：
      "the SDK brings its own nn::os, and at load time not one of these entries was resolved and used"
  也就是说——**SDK 模块里本来就实现了 nn::os**，只要把它一起装上，我们就不用自己写锁/事件/信号量。
  我们的探针至今只装 main（19 MB），而 sdk(3.0 MB)/subsdk0..3/rtld 一直躺在 exefs 里没装。
  这个脚本就是先**离线把证据拿出来**：sdk 到底有没有导出那些符号、导入了什么。

用法：
  python mod_survey.py <模块文件> --exports            # 列出导出（默认按前缀分类计数）
  python mod_survey.py <模块文件> --grep os            # 只看名字含 os 的导出
  python mod_survey.py <模块文件> --imports --grep os  # 看它导入了什么（需要 dynsym 的 UND 段）
  python mod_survey.py <模块文件> --find _ZN2nn2os11SignalEventEPNS0_9EventTypeE
"""
import argparse
import struct
import sys
from collections import Counter

try:
    import lz4.block as _lz4
except Exception:            # 沙箱里 python-lz4 时好时坏，退回纯 Python 实现
    _lz4 = None


def lz4_block_decompress(src, out_size):
    """LZ4 块格式（NSO 用的就是它）。纯 Python 版：不依赖 lz4 包，3MB 约 1-2 秒。"""
    if _lz4 is not None:
        try:
            return _lz4.decompress(src, uncompressed_size=out_size)
        except Exception:
            pass
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        if lit:
            out += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 0xF
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(out) - off
        for k in range(ml):          # 允许重叠（RLE 场景）
            out.append(out[start + k])
    if out_size and len(out) < out_size:
        out += bytes(out_size - len(out))
    return bytes(out[:out_size] if out_size else out)


def load_nso(path):
    """按 NSO0 头解出映像（三段各自可能压缩）。返回 (img, segs, bss)。"""
    d = open(path, "rb").read()
    if d[:4] != b"NSO0":
        sys.exit(f"{path} 不是 NSO0（前 4 字节 {d[:4]!r}）")
    flags = struct.unpack_from("<I", d, 0xC)[0]
    segs = [struct.unpack_from("<III", d, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from("<I", d, 0x3C)[0]
    csz = struct.unpack_from("<III", d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2] + bss)
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if flags >> i & 1:
            raw = lz4_block_decompress(raw, sz)
        img[mo:mo + sz] = raw
    return bytes(img), segs, bss


def parse_dyn(img):
    """定位 MOD0 与动态段，取出 dynsym/dynstr/hash。"""
    def u32(o): return struct.unpack_from("<I", img, o)[0]
    def u64(o): return struct.unpack_from("<Q", img, o)[0]
    mod0 = None
    for o in range(0, 0x100, 4):
        if u32(o) == 0x30444F4D:      # "MOD0"
            mod0 = o
            break
    if mod0 is None:
        sys.exit("找不到 MOD0 头")
    dyn_off = mod0 + u32(mod0 + 4)
    tags = {}
    for i in range(80):
        t, v = u64(dyn_off + i * 16), u64(dyn_off + i * 16 + 8)
        if t == 0:
            break
        tags.setdefault(t, v)
    # 9 = DT_STRSZ, 4 = DT_HASH, 5 = DT_STRTAB, 6 = DT_SYMTAB
    return tags


def sym_iter(img, tags):
    strtab, symtab = tags.get(5), tags.get(6)
    if not strtab or not symtab:
        sys.exit("没有 dynstr/dynsym")
    # DT_HASH 的表头两字：nbucket, nchain(=符号数)
    nhash = tags.get(4)
    n = struct.unpack_from("<I", img, nhash + 4)[0] if nhash else 0
    if not n:      # 退回：用 GNU_HASH 太麻烦，给个上限探测
        n = 4096
    out = []
    for i in range(n):
        st = symtab + i * 24
        name_off = struct.unpack_from("<I", img, st)[0]
        info = img[st + 4]
        shndx = struct.unpack_from("<H", img, st + 6)[0]
        value = struct.unpack_from("<Q", img, st + 8)[0]
        if not name_off:
            continue
        end = img.find(b"\0", strtab + name_off)
        nm = img[strtab + name_off:end].decode("utf-8", "replace")
        if not nm:
            continue
        out.append((nm, info, shndx, value))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mod")
    ap.add_argument("--exports", action="store_true")
    ap.add_argument("--imports", action="store_true")
    ap.add_argument("--grep", default="")
    ap.add_argument("--find", default="")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    img, segs, bss = load_nso(args.mod)
    tags = parse_dyn(img)
    syms = sym_iter(img, tags)
    print(f"# {args.mod}: 映像 {len(img):,} 字节（text {segs[0][2]:,} / ro {segs[1][2]:,} / "
          f"data {segs[2][2]:,} / bss {bss:,}）；符号 {len(syms)} 个", file=sys.stderr)

    exp = [s for s in syms if s[2] != 0]        # shndx != 0 = 已定义
    imp = [s for s in syms if s[2] == 0]
    print(f"# 导出 {len(exp)}，导入 {len(imp)}", file=sys.stderr)

    def show(lst, title):
        if args.grep:
            lst = [s for s in lst if args.grep in s[0]]
        print(f"\n== {title}（{len(lst)}）==")
        for nm, info, shndx, val in lst[:args.limit]:
            print(f"  {val:#010x}  {nm}")
        if len(lst) > args.limit:
            print(f"  …还有 {len(lst) - args.limit} 个")

    if args.find:
        hit = [s for s in syms if s[0] == args.find or args.find in s[0]]
        print(f"\n== 查找 {args.find}（{len(hit)}）==")
        for nm, info, shndx, val in hit[:20]:
            print(f"  {'导出' if shndx else '导入'} {val:#010x}  {nm}")
        return

    if args.exports or not args.imports:
        # 按 nn::XXX 的一级命名空间分类计数，先看"这模块到底管哪摊事"
        c = Counter()
        for nm, _i, _s, _v in exp:
            if nm.startswith("_ZN2nn"):
                j = nm.find("nn", 3)
                rest = nm[j + 2:]
                k = 0
                while k < len(rest) and rest[k].isdigit():
                    k += 1
                ln = int(rest[:k] or 0)
                c[rest[k:k + ln] or "?"] += 1
            else:
                c["(非 nn)" if not nm.startswith("_Z") else "(其它 C++)"] += 1
        print("\n== 导出按命名空间分类 ==")
        for k, v in c.most_common(20):
            print(f"  {v:>6}  nn::{k}")
        show(exp, "导出明细")

    if args.imports:
        show(imp, "导入明细")


if __name__ == "__main__":
    main()
