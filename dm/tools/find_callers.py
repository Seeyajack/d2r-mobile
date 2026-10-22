#!/usr/bin/env python3
"""找"谁调用/跳到某个地址"——不需要反汇编，直接扫 4 字节对齐的 BL / B 指令。

为什么不用反汇编：全映像线性反汇编会**失同步**（项目里踩过，会给出假 0）。而 AArch64 的
BL/B 是定长 4 字节、编码互不重叠：
  BL   imm26：0x94000000 起，掩码 0xFC000000 == 0x94000000
  B    imm26：0x14000000 起，掩码 0xFC000000 == 0x14000000
  BL/B 的偏移 = 符号扩展的 imm26 << 2，**相对本条指令的地址**。
所以只要按 4 字节步进扫、对每条命中算目标即可，不会失同步。

用途（2026-10-22）：确认"任务就绪位的置位函数到底有没有调用者"——np185 曾用反汇编得出"无写入者"，
这里用更硬的方式复核，并顺带找 vtable 里的函数指针（blr 调用只有它能解释）。

用法：
  python dm/tools/find_callers.py <NSO> <目标偏移(hex)> [--range 起 止]
  python dm/tools/find_callers.py <NSO> <目标偏移> --vtables     # 找数据区里指向它的 8 字节指针
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0] + '/tools')
import nso_survey as S


def scan_calls(img, text_end, targets):
    hits = {}
    for off in range(0, text_end - 4, 4):
        w = struct.unpack_from('<I', img, off)[0]
        op = w & 0xFC000000
        if op != 0x94000000 and op != 0x14000000:
            continue
        imm = w & 0x03FFFFFF
        if imm & 0x02000000:                     # 符号扩展
            imm -= 0x04000000
        dst = off + imm * 4
        if dst in targets:
            hits.setdefault(dst, []).append((off, 'BL' if op == 0x94000000 else 'B'))
    return hits


def scan_ptrs(img, lo, hi, targets):
    """数据区里指向目标的 8 字节指针（vtable 槽 / 函数指针表）"""
    hits = {}
    for off in range(lo, hi - 8, 8):
        v = struct.unpack_from('<Q', img, off)[0]
        if v in targets:
            hits.setdefault(v, []).append(off)
    return hits



def scan_plt_stubs(img, text_end, got):
    """扫 PLT 桩：`adrp x16,page` + `ldr x17,[x16,#off]`（opcode 0xF9400000，Rn=16，Rt=17）
    ⇒ 桩地址 → GOT 槽地址（再查 got 得符号名）。
    ⚠ 为什么必须有它：NX 里调用**本模块导出**是走 PLT 桩的，直接 `bl <函数体>` 会得到 0 调用者——
    2026-10-22 踩过：据此误判"sm::Initialize 没人调"。"""
    stubs = {}
    prev_adrp = None
    for off in range(0, text_end - 4, 4):
        w = struct.unpack_from('<I', img, off)[0]
        if (w & 0x9F00001F) == 0x90000010:              # adrp x16, ...
            immlo = (w >> 29) & 3
            immhi = (w >> 5) & 0x7FFFF
            imm = (immhi << 2) | immlo
            if imm & (1 << 20):
                imm -= (1 << 21)
            prev_adrp = ((off & ~0xFFF) + (imm << 12)) & 0xFFFFFFFFFFFF
            prev_at = off
            continue
        if (w & 0xFFC0001F) == 0xF9400011 and prev_adrp is not None and off - prev_at <= 8:
            imm12 = (w >> 10) & 0xFFF
            got_addr = prev_adrp + imm12 * 8
            nm = got.get(got_addr)
            if nm:
                stubs[prev_at] = nm
        prev_adrp = None
    return stubs


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path = sys.argv[1]
    targets = {int(sys.argv[2], 16)}
    for a in sys.argv[3:]:
        if a.startswith('0x'):
            targets.add(int(a, 16))
    # --flat：直接当"已装载映像"读（改写产物 main.patched.bin 就是这种，偏移与运行时一致）；
    # 否则按 NSO 解压（exefs 里的原始模块）。
    if '--flat' in sys.argv:
        img = open(path, 'rb').read()
    else:
        img = S.nso_to_image(path)
    tags = S.parse_dyn(img)
    # 文本段结束：用 DT_INIT_ARRAY 之前的部分（.text 一定在里面）；也可能更早，安全起见取整段扫
    text_end = min(len(img), tags.get('INIT_ARRAY', len(img)) or len(img))

    calls = scan_calls(img, text_end, targets)
    # PLT 桩感知：给每个"有导出的目标"加上"经 PLT 桩调用"的计数
    if '--plt' in sys.argv:
        got = {}
        if 'JMPREL' in tags:
            symtab, strtab = tags['SYMTAB'], tags['STRTAB']
            for off in range(0, tags.get('PLTRELSZ', 0), 24):
                r_off, r_info, _ = struct.unpack_from('<QQq', img, tags['JMPREL'] + off)
                symi = r_info >> 32
                if not symi:
                    continue
                nm_off = struct.unpack_from('<I', img, symtab + symi * 24)[0]
                e = img.index(b'\0', strtab + nm_off)
                got[r_off] = img[strtab + nm_off:e].decode('utf-8', 'replace')
        stubs = scan_plt_stubs(img, text_end, got)
        # 目标函数名 → 桩地址列表
        nsym = struct.unpack_from('<I', img, tags['HASH'] + 4)[0] if 'HASH' in tags else 0
        name_of = {}
        for i in range(1, nsym):
            st_off = tags['SYMTAB'] + i * 24
            n_off, info, other, shndx, val, size = struct.unpack_from('<IBBHQQ', img, st_off)
            if n_off and shndx and val in targets:
                e = img.index(b'\0', tags['STRTAB'] + n_off)
                name_of.setdefault(val, img[tags['STRTAB'] + n_off:e].decode('utf-8', 'replace'))
        for tname, addr in list(name_of.items()):
            stub_addrs = {a for a, nm in stubs.items() if nm == addr}
            if not stub_addrs:
                continue
            pc = scan_calls(img, text_end, stub_addrs)
            n = sum(len(v) for v in pc.values())
            print(f'  [PLT] {addr} 经 {len(stub_addrs)} 个桩被调用 {n} 处'
                  + ('' if not n else '：' + ', '.join(f'+{o:#x}' for v in pc.values() for o, _ in v[:4])))
    print(f'=== {path.split("/")[-1]}：扫 {text_end/1048576:.2f} MB 文本 ===')
    for t in sorted(targets):
        h = calls.get(t, [])
        print(f'  目标 +{t:#x}：{len(h)} 处直接调用' +
              ('' if not h else '：' + ', '.join(f'+{o:#x}({k})' for o, k in h[:8])))

    if '--vtables' in sys.argv:
        # 数据区：从 INIT_ARRAY 之后扫到映像尾（只读段 + 数据段都在这里）
        lo = tags.get('INIT_ARRAY', 0)
        ptrs = scan_ptrs(img, lo, len(img), targets)
        print(f'=== 数据区里的指针（vtable 槽 / 函数指针表）===')
        for t in sorted(targets):
            h = ptrs.get(t, [])
            print(f'  目标 +{t:#x}：{len(h)} 处指针' + ('' if not h else '：' + ', '.join(f'+{o:#x}' for o in h[:8])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
