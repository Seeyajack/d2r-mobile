#!/usr/bin/env python3
"""离线解出「客体 GOT 槽 → 导入名」全表。

为什么需要：客体代码里的导入桩是 `adrp x16, <槽页>; ldr x16, [x16, #<槽内偏移>]; br x16`。
崩溃回溯只给桩地址，要还原"是哪个导入"就得把桩地址翻译成槽地址，再查槽上是哪个符号。

宿主是在 relocate() 里按 DT_RELA / DT_JMPREL 填槽的（dm/host/loader.c:266 附近），
这里用同一套规则离线算一遍：NSO 头 +4 → MOD0 偏移；MOD0+4 → 动态段偏移；
动态段里取 DT_SYMTAB / DT_STRTAB / DT_RELA(+SZ) / DT_JMPREL(+PLTRELSZ)；
每条 Elf64_Rela 的 r_offset 就是**模块内地址**（NSO 段是 1:1 映射，等于文件偏移），
r_info>>32 是符号下标，符号名 = strtab + sym.st_name。

用法：
  python import_slots.py <patched.bin>                # 打印全部槽 → 名字
  python import_slots.py <patched.bin> 0x6c1b20       # 只查某个槽
  python import_slots.py <patched.bin> --stub 0x34f374  # 反汇编桩，自动算槽再查名字
"""
import struct
import sys

DT_NULL, DT_PLTRELSZ, DT_STRTAB, DT_SYMTAB, DT_RELA, DT_RELASZ, DT_JMPREL = 0, 2, 5, 6, 7, 8, 23
R_AARCH64_ABS64, R_AARCH64_GLOB_DAT, R_AARCH64_JUMP_SLOT, R_AARCH64_RELATIVE = 257, 1025, 1026, 1027


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def u64(b, o):
    return struct.unpack_from('<Q', b, o)[0]


def load_dyn(b):
    """按宿主的规则找到动态段，返回 {tag: value} 字典。"""
    mod0 = u32(b, 4)
    if b[mod0:mod0 + 4] != b'MOD0':
        raise SystemExit('不是 NSO/patched.bin：MOD0 不在 %#x' % mod0)
    dyn_off = mod0 + struct.unpack_from('<i', b, mod0 + 4)[0]
    out = {}
    p = dyn_off
    while True:
        tag, val = u64(b, p), u64(b, p + 8)
        if tag == DT_NULL:
            break
        out.setdefault(tag, val)
        p += 16
    return out


def cstr(b, off):
    end = b.find(b'\x00', off)
    return b[off:end].decode('latin1', 'replace')


def relocs(b, dyn):
    """产出 (槽地址, 类型, 符号名或 None)。顺序 = 宿主填充顺序。"""
    def one_table(off, size):
        for i in range(0, size, 24):
            r_offset, r_info, r_addend = struct.unpack_from('<QQq', b, off + i)
            rtype = r_info & 0xffffffff
            symidx = r_info >> 32
            name = None
            if symidx and DT_SYMTAB in dyn and DT_STRTAB in dyn:
                st_name = u32(b, dyn[DT_SYMTAB] + symidx * 24)
                name = cstr(b, dyn[DT_STRTAB] + st_name) if st_name else ''
            yield r_offset, rtype, name

    if DT_RELA in dyn and DT_RELASZ in dyn:
        yield from one_table(dyn[DT_RELA], dyn[DT_RELASZ])
    if DT_JMPREL in dyn and DT_PLTRELSZ in dyn:
        yield from one_table(dyn[DT_JMPREL], dyn[DT_PLTRELSZ])


def stub_slot(b, stub):
    """反汇编导入桩，返回它读的那个 GOT 槽地址（adrp + ldr 组合）。"""
    try:
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
    except ImportError:
        raise SystemExit('需要 capstone：请用 ~/.workbuddy/binaries/python/envs/default/Scripts/python.exe')
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    page, imm = None, 0
    for ins in md.disasm(b[stub:stub + 0x20], stub):
        if ins.mnemonic == 'adrp':
            page = int(ins.op_str.split('#')[-1], 0)
        elif ins.mnemonic == 'ldr' and page is not None:
            try:
                imm = int(ins.op_str.split('#')[-1].rstrip(']'), 0)
            except ValueError:
                imm = 0
            return page + imm
        elif ins.mnemonic in ('br', 'blr'):
            break
    return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    b = open(sys.argv[1], 'rb').read()
    dyn = load_dyn(b)
    table = list(relocs(b, dyn))

    if len(sys.argv) >= 3 and sys.argv[2] == '--stub':
        stub = int(sys.argv[3], 0)
        slot = stub_slot(b, stub)
        print('桩 %#x → 槽 %#x' % (stub, slot or 0))
        hits = [(o, t, n) for o, t, n in table if o == slot]
        if not hits:
            print('  槽上没有重定位（说明这个槽不来自导入）')
        for o, t, n in hits:
            print('  %#x type=%d  %s' % (o, t, n))
        return

    if len(sys.argv) >= 3:
        want = int(sys.argv[2], 0)
        for o, t, n in table:
            if o == want:
                print('%#x type=%d  %s' % (o, t, n))
        return

    named = [(o, t, n) for o, t, n in table if n]
    print('# 重定位共 %d 条，带符号名的 %d 条' % (len(table), len(named)))
    print('# 槽地址\t类型\t符号')
    for o, t, n in named:
        print('%#x\t%d\t%s' % (o, t, n))


if __name__ == '__main__':
    main()
