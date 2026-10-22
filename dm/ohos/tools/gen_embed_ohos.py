#!/usr/bin/env python3
# 生成内嵌汇编：把改写后的模块映像编进 libdmohos.so，放进自定义 ELF 段。
#
#   dm<名>x0   原 text：[0, 圆整16K(TEXT_END))
#   dm<名>w    ro 余部 + data + bss：[圆整16K(TEXT_END), PATCH_OFF)（重定位只落这里）
#   dm<名>x1   补丁区：[PATCH_OFF, 圆整16K(文件尾))
#
# 运行时由 dm/ohos/map_image_ohos.c 找到这三段、拷进一块匿名内存、
# 再把代码区 mprotect 成只读+可执行 —— 布局与原映像逐字节相同，
# 所以原代码与 data 之间的 PC 相对寻址、补丁区的相对跳转、
# 宿主按「基址 + 原偏移」的引用都不用动。
#
# 与上游 dm/ios/tools/gen_embed.py 的区别：
#   1. 段名换成合法 C 标识符 —— 链接器才会为它生成 __start_<段名> / __stop_<段名>
#      （真机实验 C 已验证）
#   2. 三块只要 "a"（可分配），**不需要可执行段** ——
#      运行时是拷进匿名内存再改权限（真机实验 A 已验证），段本身只读即可
#   3. 不需要 -segprot 之类的链接参数
#
# 用法：gen_embed_ohos.py [--embed-bytes] <产物目录> <输出 .S> <模块名...>
#
#   --embed-bytes  不写 .incbin，而是把字节直接展开成 .byte/.space。
#                  产物自包含、可提交进仓库（真机测试用）；连续零字节用 .space 压缩。
#   --layout-header <输出.h>  把 fixups 表头（PATCH_OFF/TEXT_END/…）写成 #define，
#                  供探针组装 module_t（运行时装载要用这些布局常数）。
#   --xmods-header <输出.h>   把"额外模块"装载表（除 main/sdk 外）写成常量数组，
#                  供探针 dm_probe.c 初始化 g_xmods——**换游戏只换这个文件，不动 C 代码**。
#   --gates-header <输出.h>   把 <名>.gates.tsv（门表：返回点/kind/arg/原指令）写成登记表数组，
#                  供探针在装载后逐条 gate_register（执行链的门回路验证用）。
import os
import sys

PG = 0x4000          # 16 KB：与改写器产物、map_image_ohos.c 的取整口径一致
SECTIONS = (('x0', 0, 'a'), ('w', 1, 'a'), ('x1', 2, 'a'))
BYTES_PER_LINE = 16


def r16(x):
    return (x + PG - 1) & ~(PG - 1)


def emit_bytes(asm, data, base_off, total):
    """把 data（可短于 total）按 .byte/.space 展开：连续零字节压成 .space。"""
    asm.append(f'    // 偏移 {base_off:#x} 起 {len(data)} 字节，其余补零到 {total:#x}')
    i, n = 0, len(data)
    while i < n:
        if data[i] == 0:
            j = i
            while j < n and data[j] == 0:
                j += 1
            asm.append(f'    .space {j - i}')
            i = j
            continue
        j = i
        while j < n and j - i < BYTES_PER_LINE:
            j += 1
        asm.append('    .byte ' + ', '.join(f'{b:#04x}' for b in data[i:j]))
        i = j
    if total > n:
        asm.append(f'    .space {total - n}')


def read_hdr(path):
    """读改写器产物表头：每行形如 '# KEY VALUE'。"""
    hdr = {}
    with open(path, 'r', encoding='utf-8') as f:
        for line in f:
            if not line.startswith('# '):
                break
            parts = line[2:].split()
            if len(parts) == 2:
                hdr[parts[0]] = parts[1]
    return hdr


def gen_one(out_dir, name, asm, embed_bytes):
    hdr = read_hdr(os.path.join(out_dir, f'{name}.fixups.tsv'))
    for k in ('TEXT_END', 'PATCH_OFF', 'KEY_PAGE_OFF', 'KEY_PAGE_SIZE'):
        if k not in hdr:
            sys.exit(f'【红】{name}.fixups.tsv 缺 {k}')

    te = int(hdr['TEXT_END'], 16)
    po = int(hdr['PATCH_OFF'], 16)
    ko = int(hdr['KEY_PAGE_OFF'], 16)
    ks = int(hdr['KEY_PAGE_SIZE'], 16)

    # 用正斜杠：反斜杠在汇编字符串里会被当转义符
    bin_path = os.path.abspath(os.path.join(out_dir, f'{name}.patched.bin')).replace('\\', '/')
    sz = os.path.getsize(bin_path)
    a, end = r16(te), r16(sz)
    raw = open(bin_path, 'rb').read() if embed_bytes else None

    # 布局自检：与 map_image_ohos.c 的 alloc 里那几条对应
    if po % PG:
        sys.exit(f'【红】{name}: PATCH_OFF {po:#x} 未按 16K 对齐')
    if not (a <= ko and ko + ks <= po):
        sys.exit(f'【红】{name}: 键页 [{ko:#x}, {ko+ks:#x}) 不在 [{a:#x}, {po:#x}) 内')
    if ko % PG or ks != PG:
        sys.exit(f'【红】{name}: 键页偏移/大小不对（{ko:#x} / {ks:#x}）')
    if end < po:
        sys.exit(f'【红】{name}: 文件尾 {end:#x} 早于 PATCH_OFF {po:#x}')

    asm.append(f'// ---- 模块 {name}：TEXT_END {te:#x} → 取整 {a:#x}，PATCH_OFF {po:#x}，'
               f'文件 {sz} 字节 → 取整 {end:#x} ----')
    for suf, idx, flags in SECTIONS:
        sect = f'dm{name}{suf}'
        if not sect.isidentifier():
            sys.exit(f'【红】段名 {sect} 不是合法 C 标识符（链接器不会生成 __start_ 符号）')
        lo, hi = (0, a) if idx == 0 else ((a, po) if idx == 1 else (po, end))
        asm.append(f'    .section {sect}, "{flags}", @progbits')
        asm.append('    .p2align 14')
        if embed_bytes:
            emit_bytes(asm, raw[lo:min(hi, sz)], lo, hi - lo)
        else:
            fill = min(hi, sz) - lo
            asm.append(f'    .incbin "{bin_path}", {lo:#x}, {fill:#x}')
            if hi - lo > fill:
                asm.append(f'    .space {hi - lo - fill:#x}')   # bss 部分补零
        asm.append('    .p2align 14')
    return a, po, end


def gen_xmods(out_dir, names, dst):
    """额外模块装载表 → C 常量数组（dm_probe.c 的 g_xmods 数据源）。

    为什么：g_xmods 以前在 dm_probe.c 里手写，每行引用 DM_IMG_<名>_* 宏——换一款游戏
    缺一个 subsdk 就直接编译不过（宏未定义）。改成**生成的数据表**后，换游戏只是
    重新生成本文件，C 代码不动。main/sdk 除外：那两个在探针里有定制装载路径
    （k_load_sdk / exp_k），不走通用表。
    """
    xmods = [n for n in names if n not in ('main', 'sdk')]
    lines = ['// 【生成文件】由 gen_embed_ohos.py --xmods-header 生成，勿手改。',
             '// 运行时"额外模块"表：除 main/sdk（定制装载）外的全部模块，顺序 = 装载顺序。',
             '// 布局常量来自 dm_main_layout.h（同目录，由 --layout-header 一起生成）。',
             '#pragma once',
             '#include "dm_main_layout.h"',
             '',
             f'#define DM_XMODS_GEN_N {len(xmods)}',
             '// 字段与 dm_probe.c 的 xmod_t 一一对应（name + module_t 的 8 个布局常数）',
             'static const struct {',
             '    const char *name;',
             '    uint64_t text_end, patch_off, ro_off, data_off, key_page_off, key_page_size, x0_end, img_end;',
             '} dm_xmods_gen[] = {']
    if xmods:
        for n in xmods:
            up = n.upper()
            lines.append(f'    {{ "{n}", DM_IMG_{up}_TEXT_END, DM_IMG_{up}_PATCH_OFF, DM_IMG_{up}_RO_OFF,')
            lines.append(f'      DM_IMG_{up}_DATA_OFF, DM_IMG_{up}_KEY_PAGE_OFF, DM_IMG_{up}_KEY_PAGE_SIZE,')
            lines.append(f'      DM_IMG_{up}_X0_END, DM_IMG_{up}_END }},')
    else:
        # 零长度数组不是合法 C ⇒ 给一条哑元（探针只读前 DM_XMODS_GEN_N 条，N=0 时不碰它）
        lines.append('    { "", 0, 0, 0, 0, 0, 0, 0, 0 },   // N=0：本游戏只有 main/sdk，无额外模块')
    lines.append('};')
    with open(dst, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'已写出 {dst}（额外模块 {len(xmods)} 个：{", ".join(xmods) if xmods else "无"}）')


def main():
    argv = sys.argv[1:]
    embed_bytes = '--embed-bytes' in argv
    layout_hdr = gates_hdr = xmods_hdr = None
    if '--layout-header' in argv:
        k = argv.index('--layout-header')
        layout_hdr = argv[k + 1]
        del argv[k:k + 2]
    if '--xmods-header' in argv:
        k = argv.index('--xmods-header')
        xmods_hdr = argv[k + 1]
        del argv[k:k + 2]
    if '--gates-header' in argv:
        k = argv.index('--gates-header')
        gates_hdr = argv[k + 1]
        del argv[k:k + 2]
    if embed_bytes:
        argv.remove('--embed-bytes')
    if len(argv) < 3:
        sys.exit('用法：gen_embed_ohos.py [--embed-bytes] <产物目录> <输出 .S> <模块名...>')
    out_dir, dst, names = argv[0], argv[1], argv[2:]

    asm = [
        '// 由 dm/ohos/tools/gen_embed_ohos.py 生成，勿手改',
        '// 段名必须是合法 C 标识符：链接器据此生成 __start_<段名> / __stop_<段名>。',
        '// 三块都只要 "a"（可分配）—— 运行时拷进匿名内存再 mprotect 成 R+X。',
    ]
    total = 0
    for n in names:
        _, po, end = gen_one(out_dir, n, asm, embed_bytes)
        total += end
        print(f'  {n}: 段 dm{n}x0 / dm{n}w / dm{n}x1，合计 {end} 字节（{end/PG:.1f} 个 16K 页）')

    with open(dst, 'w', encoding='utf-8') as f:
        f.write('\n'.join(asm) + '\n')
    print(f'已写出 {dst}')
    print(f'合计 {total} 字节（{total/1024/1024:.2f} MiB），将全部进入 .so')

    if layout_hdr:
        # 布局常数头：fixups 表头的 KEY → #define（探针组装 module_t 用）。
        # 常量名 = DM_IMG_<模块名大写>_<KEY 大写>；换代/改写器变更后随 .S 一起重新生成。
        lines = ['// 【生成文件】由 gen_embed_ohos.py --layout-header 生成，勿手改。',
                 '// 来源：各模块 <名>.fixups.tsv 表头（改写器输出的布局与键页表）。',
                 '#pragma once', '']
        for n in names:
            hdr = read_hdr(os.path.join(out_dir, f'{n}.fixups.tsv'))
            up = n.upper()
            for key, val in hdr.items():
                kk = key.replace(' ', '_').upper()
                lines.append(f'#define DM_IMG_{up}_{kk} {val}')
            # 段的**精确**范围（.S 三块的边界）：装载拷贝用它，不信任 ELF 段大小
            # （lld 会给每段尾垫一个对齐页）。gen_one 返回 (x0 结束, PATCH_OFF, 映像结束)。
            a, po, end = gen_one(out_dir, n, [], False)
            lines.append(f'#define DM_IMG_{up}_X0_END {a:#x}')
            lines.append(f'#define DM_IMG_{up}_END {end:#x}')
            lines.append('')
        with open(layout_hdr, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        print(f'已写出 {layout_hdr}')

    if xmods_hdr:
        gen_xmods(out_dir, names, xmods_hdr)

    if gates_hdr:
        # 门表登记数组：{返回点（分派键，门块内 blr 之后）, 原指令地址, kind 码, arg}
        # kind 码与 hle.c 的枚举一致：0=svc 1=cntpct 2=cntvct。调用方按码转回字符串给 gate_register。
        glines = ['// 【生成文件】由 gen_embed_ohos.py --gates-header 生成，勿手改。',
                  '// 来源：各模块 <名>.gates.tsv（改写器输出的门表）。',
                  '#pragma once', '']
        for n in names:
            gp = os.path.join(out_dir, f'{n}.gates.tsv')
            gates = []
            for ln in open(gp, encoding='utf-8'):
                if ln.startswith('#') or not ln.strip(): continue
                w = ln.split()
                if len(w) >= 4:
                    kind = {'svc': 0, 'cntpct': 1, 'cntvct': 2}.get(w[1], -1)
                    gates.append((int(w[0], 16), kind, int(w[2]), int(w[3], 16)))
            up = n.upper()
            glines.append(f'#define DM_GATES_{up}_N {len(gates)}')
            glines.append('static const struct { uint64_t ret; uint64_t orig; int kind; int arg; } '
                          f'DM_GATES_{up}[] = ' + '{')
            for ret, kind, arg, orig in gates:
                glines.append(f'    {{{ret:#x}, {orig:#x}, {kind}, {arg}}},')
            glines.append('};')
            glines.append('')
        with open(gates_hdr, 'w', encoding='utf-8') as f:
            f.write('\n'.join(glines) + '\n')
        print(f'已写出 {gates_hdr}')


if __name__ == '__main__':
    main()
