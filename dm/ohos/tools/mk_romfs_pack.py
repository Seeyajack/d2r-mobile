#!/usr/bin/env python3
"""把 romfs 的一个子集打成单文件归档，供固化进 .so（dm_romfs.S 的 .incbin 用）。

为什么需要：鸿蒙应用的沙箱**看不到 /data/local/tmp**（真机实证：access 返回 ENOENT），
而 hdc 是 shell 身份、又写不进应用沙箱 ⇒ 唯一稳定的投递方式是把数据**放进 HAP**
（这里选择嵌进 .so，复用 main 内嵌那套 .incbin 机制，构建快、无运行时拷贝）。

格式（小端）：
  0x00  magic  "DMRF"
  0x04  u32    version = 1
  0x08  u32    count
  0x0C  u32    reserved
  0x10  entries[count]：{ char path[0x100]; u64 off; u64 size; }   // 0x110 字节/条
  ...   数据区（off 相对归档起点，16 字节对齐）

用法：
  mk_romfs_pack.py <romfs根目录> <输出.bin> <包含前缀...>
  例：mk_romfs_pack.py F:/switch-work/nca_full/romfs F:/switch-work/nca_full/romfs_boot.pack \
        System/ Pack/Bootup
"""
import os
import struct
import sys

ENT = 0x110
MAGIC = b"DMRF"


def walk(root, prefixes):
    out = []
    for dirpath, _dirs, files in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace("\\", "/")
        if rel_dir == ".":
            rel_dir = ""
        for f in files:
            rel = (rel_dir + "/" + f) if rel_dir else f
            if any(rel.startswith(p) for p in prefixes):
                out.append(rel)
    out.sort()
    return out


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    root, out_path = argv[1], argv[2]
    prefixes = argv[3:]
    files = walk(root, prefixes)
    if not files:
        print("没有文件匹配前缀：", prefixes)
        return 1
    total = 0
    entries = []
    for rel in files:
        p = os.path.join(root, rel)
        sz = os.path.getsize(p)
        entries.append((rel, total, sz))
        total += sz + (-sz % 16)          # 16 字节对齐
    hdr_len = 0x10 + ENT * len(entries)
    with open(out_path, "wb") as w:
        w.write(MAGIC)
        w.write(struct.pack("<III", 1, len(entries), 0))
        for rel, off, sz in entries:
            key = rel.encode("utf-8")[:0xFF]
            w.write(key + b"\x00" * (0x100 - len(key)))
            w.write(struct.pack("<QQ", hdr_len + off, sz))
        for rel, off, sz in entries:
            p = os.path.join(root, rel)
            with open(p, "rb") as r:
                w.write(r.read())
            pad = (-sz) % 16
            if pad:
                w.write(b"\x00" * pad)
    print("归档 %s：%d 个文件，数据 %.1f MB，文件总 %.1f MB" %
          (out_path, len(entries), total / 1048576, os.path.getsize(out_path) / 1048576))
    for rel, off, sz in entries[:5]:
        print("   %-46s %8d B" % (rel, sz))
    if len(entries) > 5:
        print("   … 共 %d 个" % len(entries))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
