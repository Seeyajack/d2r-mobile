#!/usr/bin/env python3
"""算出「客体要宿主提供、而宿主还没有」的导入清单 —— 换游戏时前方的雷区地图。

背景：宿主的 resolve_import（dm/host/hle.c）只有两条路——
  ① 查 swhle 段（= hle_*.c 的 HLE/HLE_FORCE 条目 + hle_table.c 的 libc 直通表）
  ② 查不到就发一个"未实现桩"，**调用恒返回 0**
客体把 0 当指针用时就会 SIGSEGV。所以这份清单 = 未来的崩溃点列表，
每跑一轮把清单划掉一条，就是"离出画面"的可量化进度。

判定规则：
  · 客体模块内定义的符号（dynsym st_shndx != 0）不需要宿主；
  · 两个模块（main / sdk）之间互相满足的也不算；
  · 弱导入（st_info 绑定 = STB_WEAK）返回 0 是预期行为，单列，不算雷。
  · 其余 = 落在未实现桩上 ⇒ 雷。

用法：
  python import_audit.py <main.patched.bin> <sdk.patched.bin> <host 源码目录> [--tsv out.tsv]
"""
import os
import re
import struct
import sys

STB_WEAK = 2
DT_NULL, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_JMPREL, DT_PLTRELSZ = 0, 5, 6, 7, 8, 23, 2


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def u64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def load_dyn(b):
    mod0 = u32(b, 4)
    if b[mod0:mod0 + 4] != b'MOD0':
        raise SystemExit('不是 NSO：MOD0 不在 %#x' % mod0)
    dyn_off = mod0 + struct.unpack_from('<i', b, mod0 + 4)[0]
    out, p = {}, dyn_off
    while True:
        tag, val = u64(b, p), u64(b, p + 8)
        if tag == DT_NULL:
            break
        out.setdefault(tag, val)
        p += 16
    return out


def symbols(b, dyn):
    """产出 (名字, 是否本模块定义, 绑定)。dynsym 按 24 字节一项，靠 strtab 定界。"""
    symtab, strtab = dyn.get(DT_SYMTAB), dyn.get(DT_STRTAB)
    if symtab is None or strtab is None:
        return
    # dynsym 长度没有直接标签，取到 strtab 起点（strtab 在 dynsym 之后）
    n = (strtab - symtab) // 24 if strtab > symtab else 4096
    for i in range(n):
        base = symtab + i * 24
        st_name = u32(b, base)
        st_info = b[base + 4]
        st_shndx = struct.unpack_from('<H', b, base + 6)[0]
        if not st_name:
            continue
        end = b.find(b'\x00', strtab + st_name)
        yield b[strtab + st_name:end].decode('latin1', 'replace'), st_shndx != 0, st_info >> 4


def imports(b, dyn):
    """产出 (槽地址, 名字, 是否弱)。只取带符号名的重定位。"""
    def one(off, size):
        for i in range(0, size, 24):
            r_offset, r_info = struct.unpack_from('<QQ', b, off + i)
            symidx = r_info >> 32
            if not symidx:
                continue
            base = dyn[DT_SYMTAB] + symidx * 24
            st_name = u32(b, base)
            if not st_name:
                continue
            st_info = b[base + 4]
            end = b.find(b'\x00', dyn[DT_STRTAB] + st_name)
            yield r_offset, b[dyn[DT_STRTAB] + st_name:end].decode('latin1', 'replace'), (st_info >> 4) == STB_WEAK

    if DT_RELA in dyn and DT_RELASZ in dyn:
        yield from one(dyn[DT_RELA], dyn[DT_RELASZ])
    if DT_JMPREL in dyn and DT_PLTRELSZ in dyn:
        yield from one(dyn[DT_JMPREL], dyn[DT_PLTRELSZ])


def host_provided(srcdir):
    """从宿主源码里抽出 swhle 段的全部条目名。"""
    names = set()
    pat_hle = re.compile(r'HLE(?:_FORCE)?\(\s*"([^"]+)"')
    pat_lib = re.compile(r'\bL\(([A-Za-z_][A-Za-z0-9_]*)\)')
    for root, _dirs, files in os.walk(srcdir):
        for fn in files:
            if not fn.endswith(('.c', '.cpp', '.h')):
                continue
            s = open(os.path.join(root, fn), encoding='utf-8', errors='replace').read()
            names.update(pat_hle.findall(s))
            names.update(pat_lib.findall(s))
    return names


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    mods = {}
    for path in sys.argv[1:3]:
        b = open(path, 'rb').read()
        dyn = load_dyn(b)
        defined = {n for n, isdef, _ in symbols(b, dyn) if isdef}
        imps = list(imports(b, dyn))
        mods[os.path.basename(path)] = (b, dyn, defined, imps)

    provided = host_provided(sys.argv[3])
    tsv = sys.argv[5] if len(sys.argv) > 5 and sys.argv[4] == '--tsv' else None

    rows = []
    for mname, (b, dyn, defined, imps) in mods.items():
        others = set()
        for oname, (_, _, odef, _) in mods.items():
            if oname != mname:
                others |= odef
        for slot, name, weak in imps:
            if name in defined or name in others:
                continue          # 客体自己/别的模块能提供
            if name in provided:
                continue          # 宿主有实现
            rows.append((mname, slot, name, 'weak' if weak else 'STRONG'))

    strong = [r for r in rows if r[3] == 'STRONG']
    weak = [r for r in rows if r[3] == 'weak']
    print('# 宿主缺的导入：强引用 %d 条（会崩），弱引用 %d 条（返回 0 是预期）' % (len(strong), len(weak)))

    def bucket(n):
        table = (('nn::', ('_ZN2nn', '_ZNK2nn', '_ZN2nn')), ('nv::', ('_ZN2nv', '_ZNK2nv')),
                 ('std::', ('_ZNSt', '_ZNKSt', '_ZSt')), ('movie::', ('_ZN5movie', '_ZNK5movie')),
                 ('sead::', ('_ZN4sead', '_ZNK4sead')), ('vlt::', ('_ZN3vlt', '_ZNK3vlt')),
                 ('al::', ('_ZN2al', '_ZNK2al')))
        for label, pres in table:
            if n.startswith(pres):
                return label
        return '其他（C 符号 / 未归类的 C++）'

    groups = {}
    for r in strong:
        groups.setdefault(bucket(r[2]), []).append(r)
    for k in sorted(groups, key=lambda x: -len(groups[x])):
        print('\n## %s（%d 条）' % (k, len(groups[k])))
        for _, slot, name, _ in groups[k][:40]:
            print('   %#08x  %s' % (slot, name))
        if len(groups[k]) > 40:
            print('   …… 还有 %d 条' % (len(groups[k]) - 40))

    if tsv:
        with open(tsv, 'w', encoding='utf-8') as f:
            f.write('# module\tslot\tname\tbind\n')
            for m, slot, name, bind in rows:
                f.write('%s\t%#x\t%s\t%s\n' % (m, slot, name, bind))
        print('\n（明细已写 %s）' % tsv)


if __name__ == '__main__':
    main()
