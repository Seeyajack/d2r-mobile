#!/usr/bin/env python3
"""NSO 函数调用分析：给定「模块 + 偏移」，反汇编并解析每条调用到**哪个符号**。

为什么需要它（2026-10-22，用户要求"离线搞清楚再上机"）：
  真机上一个构造函数卡死，只给出"入口地址"；要判断它卡在哪个服务/驱动调用上，必须离线把
  入口那段反汇编、并把 `bl` 目标翻译成符号——否则只能一遍遍上机试。

解析规则（NX 的 AArch64 PLT）：
  · 每个导入在 .got 里有一槽（JUMP_SLOT 重定位的 r_offset 就是这槽的模块内偏移），槽里是符号名；
  · 调导入时走一小段桩：`adrp x16,page; ldr x17,[x16,#lo]; add x16,x16,#lo; br x17`，
    桩里算出的是**同一个 GOT 槽** ⇒ 用"桩算出的地址"反查符号名。
用法：
  python dm/tools/nso_calls.py <NSO> <偏移(hex)> [--bytes 0x400] [--depth 2]
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0] + '/tools')
import nso_survey as S            # 复用 NSO 解压 + 动态段解析
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM


def build_maps(img, tags):
    """返回 (got槽→符号名, 导出地址→名字列表)"""
    symtab, strtab = tags['SYMTAB'], tags['STRTAB']
    nsym = struct.unpack_from('<I', img, tags['HASH'] + 4)[0] if 'HASH' in tags else 0

    def name_at(off):
        e = img.index(b'\0', strtab + off)
        return img[strtab + off:e].decode('utf-8', 'replace')

    got = {}
    if 'JMPREL' in tags:
        for off in range(0, tags['PLTRELSZ'], 24):
            r_off, r_info, _ = struct.unpack_from('<QQq', img, tags['JMPREL'] + off)
            symi = r_info >> 32
            if not symi:
                continue
            nm = name_at(struct.unpack_from('<I', img, symtab + symi * 24)[0])
            got[r_off] = nm
    if 'RELA' in tags:
        for off in range(0, tags['RELASZ'], 24):
            r_off, r_info, _ = struct.unpack_from('<QQq', img, tags['RELA'] + off)
            if (r_info & 0xFFFFFFFF) != 1025:
                continue
            symi = r_info >> 32
            if not symi:
                continue
            nm = name_at(struct.unpack_from('<I', img, symtab + symi * 24)[0])
            got.setdefault(r_off, nm)

    exports = {}
    for i in range(1, nsym):
        st = symtab + i * 24
        nm_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, st)
        if nm_off and shndx and value:
            exports.setdefault(value, []).append(name_at(nm_off))
    return got, exports


def plt_stubs(img, got, text_end):
    """扫出 PLT 桩：adrp x16 / ldr x17 / add x16 / br x17 ⇒ 算出 GOT 槽地址，反查符号名"""
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    stubs = {}
    n = min(text_end, 0x2000)
    ins = list(md.disasm(img[:n], 0))
    for i in range(len(ins) - 3):
        a, b, c, d = ins[i:i + 4]
        if (a.mnemonic == 'adrp' and b.mnemonic == 'ldr' and c.mnemonic == 'add' and d.mnemonic == 'br'):
            try:
                page = int(a.op_str.split('#')[1], 16)
                imm = b.op_str.split('#')[1].rstrip(']')
                off = int(imm, 16) if not imm.startswith('-') else -int(imm[1:], 16)
            except Exception:
                continue
            got_addr = page + off
            if got_addr in got:
                stubs[a.address] = got[got_addr]
    return stubs


def resolve_stub(md, img, got, addr):
    """按需解析"这个地址是不是 PLT 桩"：adrp x16 / ldr x17,[x16,#x] / add x16,x16,#x / br x17
    用正则抠十六进制数（capstone 的 op_str 里带方括号、逗号，直接 split 会崩——踩过）。"""
    import re
    ins = list(md.disasm(img[addr:addr + 16], addr))
    if len(ins) < 4:
        return None
    a, b, c, d = ins[:4]
    if a.mnemonic != 'adrp' or b.mnemonic != 'ldr' or c.mnemonic != 'add' or d.mnemonic != 'br':
        return None
    if 'x16' not in a.op_str or 'x17' not in b.op_str or 'x17' not in d.op_str:
        return None
    nums_a = re.findall(r'0x[0-9a-f]+', a.op_str)
    nums_b = re.findall(r'#(-?0x[0-9a-f]+)', b.op_str)
    if not nums_a or not nums_b:
        return None
    page = int(nums_a[-1], 16)
    off = int(nums_b[-1], 16)
    return got.get(page + off)



def walk(md, img, got, exports, addr, depth, out, budget=[400], name2addr=None):
    """从 addr 起反汇编，打印调用树（深度受限）。返回遇到的"导入名"列表。"""
    import re
    if depth < 0 or budget[0] <= 0:
        return []
    ins = list(md.disasm(img[addr:addr + 0x200], addr))
    seen = []
    for k, i in enumerate(ins):
        if i.mnemonic == 'ret' or (i.mnemonic == 'b' and k > 4):
            break
        if i.mnemonic != 'bl' or not i.op_str.startswith('#'):
            continue
        tgt = int(i.op_str[1:], 16)
        sym = resolve_stub(md, img, got, tgt)
        if sym:
            # 导入名如果在**本模块也导出**（NX 的内线调用全走 PLT），继续往下追
            addr2 = (name2addr or {}).get(sym)
            if addr2 is not None and budget[0] > 0:
                out.append('  ' * (3 - depth) + f'→ {sym}（同模块）')
                budget[0] -= 1
                walk(md, img, got, exports, addr2, depth - 1, out, budget, name2addr)
            else:
                out.append('  ' * (3 - depth) + f'→ 导入 {sym}')
            seen.append(sym)
            continue
        if tgt in exports:
            nm = exports[tgt][0]
            out.append('  ' * (3 - depth) + f'→ {nm}')
            budget[0] -= 1
            walk(md, img, got, exports, tgt, depth - 1, out, budget, name2addr)
        else:
            out.append('  ' * (3 - depth) + f'→ +{tgt:#x}（未导出）')
            budget[0] -= 1
            walk(md, img, got, exports, tgt, depth - 1, out, budget, name2addr)
    return seen

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, start = sys.argv[1], int(sys.argv[2], 16)
    nbytes = int(sys.argv[sys.argv.index('--bytes') + 1], 16) if '--bytes' in sys.argv else 0x300
    img = S.nso_to_image(path)
    tags = S.parse_dyn(img)
    got, exports = build_maps(img, tags)
    text_end = tags.get('INIT_ARRAY', 0) or 0x2000
    stubs = plt_stubs(img, got, 0x2000)

    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    if '--walk' in sys.argv:
        n2a = {}
        for off, names in exports.items():
            for nm in names:
                n2a.setdefault(nm, off)
        out = []
        walk(md, img, got, exports, start, 3, out, [600], n2a)
        print(f'=== {path.split("/")[-1]}+{start:#x} 调用树 ===')
        print('\n'.join(out))
        return 0
    print(f'=== {path.split("/")[-1]}+{start:#x}（PLT 桩 {len(stubs)} 个，GOT 槽 {len(got)} 个）===')
    for ins in md.disasm(img[start:start + nbytes], start):
        line = f'{ins.address:#08x}: {ins.mnemonic:<7}{ins.op_str}'
        if ins.mnemonic == 'bl' and ins.op_str.startswith('#'):
            tgt = int(ins.op_str[1:], 16)
            sym = stubs.get(tgt) or resolve_stub(md, img, got, tgt)
            if sym:
                line += f'   ← 导入 {sym}'
            elif tgt in exports:
                line += f'   ← 本模块 {exports[tgt][0]}'
            else:
                line += f'   → +{tgt:#x}'
        elif ins.mnemonic == 'blr':
            line += '   ← 间接调用（虚/函数指针）'
        print(line)
        if ins.mnemonic == 'ret':
            break
    return 0


if __name__ == '__main__':
    sys.exit(main())
