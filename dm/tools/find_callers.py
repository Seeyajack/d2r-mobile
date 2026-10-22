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
