#!/usr/bin/env python3
"""NSO 模块勘察：解开 6 个模块，列出各自的**导出 / 导入 / 构造函数**，并算出跨模块解析表。

为什么要有它（2026-10-22）：
  我们的探针只装 `main`（+ `sdk`），而 nxnative 那套是**装全部模块**（rtld/sdk/subsdk0-3/main），
  让 SDK 自带的 nn::os 跑起来——这也解释了"任务 runner 的 flags 没人置位"：置位者可能在
  我们没加载的模块里。要动这一刀，先得知道：谁导出什么、谁导入什么、每个模块有多少构造函数。

实现要点：
  · NSO 的三段可能是 LZ4 压缩的（flags 的 bit0/1/2）⇒ 这里带一个**纯 Python 的 LZ4 block 解码器**，
    不依赖 python-lz4（本机没装，也不想为一次勘察装包）。
  · ELF 动态段从 MOD0 开始（NX 的约定），DT_HASH 给符号数（nchain），DT_JMPREL 给导入。
用法：
  python dm/tools/nso_survey.py <exefs 目录> [--json 输出.json]
"""
import json
import os
import struct
import sys


# ───────────────────────── 纯 Python LZ4 block 解码 ─────────────────────────
def lz4_block_decompress(src: bytes, out_size: int) -> bytes:
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
        dst += src[i:i + lit]; i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        mlen = token & 0xF
        if mlen == 15:
            while True:
                b = src[i]; i += 1; mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(dst) - off
        if start < 0:
            raise ValueError("LZ4 偏移越界")
        for k in range(mlen):                     # 逐字节拷（重叠拷贝必须这样）
            dst.append(dst[start + k])
    if len(dst) != out_size:
        # 有些产物尾部补齐；长度不符就按实际长度返回，交给调用方判断
        pass
    return bytes(dst)


def nso_to_image(path: str) -> bytes:
    d = open(path, 'rb').read()
    flags = struct.unpack_from('<I', d, 0x0C)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from('<I', d, 0x3C)[0]
    csz = struct.unpack_from('<III', d, 0x60)
    end = segs[2][1] + segs[2][2]
    img = bytearray(end + bss)
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if (flags >> i) & 1:
            raw = lz4_block_decompress(raw, sz)
        img[mo:mo + sz] = raw[:sz]
    return bytes(img)


# ───────────────────────── ELF 动态段 ─────────────────────────
DT = {0: 'NULL', 2: 'PLTRELSZ', 4: 'HASH', 5: 'STRTAB', 6: 'SYMTAB', 7: 'RELA', 8: 'RELASZ',
      0x17: 'JMPREL', 25: 'INIT_ARRAY', 27: 'INIT_ARRAYSZ', 0x6ffffff9: 'RELACOUNT'}


def parse_dyn(img: bytes):
    # MOD0 的位置不固定：main/sdk 在映像很前面，rtld 却在 0x441C（踩过：按 0x100 扫会找不到）。
    # 全映像找第一处「MOD0 + dynamic 偏移落在映像内」的组合。
    mod0 = None
    pos = 0
    while True:
        pos = img.find(b'MOD0', pos)
        if pos < 0 or pos % 4:
            if pos < 0:
                break
            pos += 1
            continue
        dyn_off = struct.unpack_from('<I', img, pos + 4)[0]
        if 0 < dyn_off < len(img):
            mod0 = pos
            break
        pos += 1
    if mod0 is None:
        raise ValueError("找不到 MOD0")
    dyn = mod0 + struct.unpack_from('<I', img, mod0 + 4)[0]
    tags = {}
    for i in range(200):
        tag, val = struct.unpack_from('<QQ', img, dyn + i * 16)
        if tag == 0:
            break
        tags.setdefault(DT.get(tag, hex(tag)), val)
    return tags


def symbols(img: bytes, tags):
    """返回 (导出列表[(name, value)], 导入列表[(name, reloc_type)])"""
    strtab = tags.get('STRTAB', 0)
    symtab = tags.get('SYMTAB', 0)
    nsym = 0
    if 'HASH' in tags:
        nsym = struct.unpack_from('<I', img, tags['HASH'] + 4)[0]     # nchain == 符号数

    def name_at(off):
        e = img.index(b'\0', strtab + off)
        return img[strtab + off:e].decode('utf-8', 'replace')

    exports = []
    for i in range(1, nsym):
        st = symtab + i * 24
        nm_off, info, other, shndx, value, size = struct.unpack_from('<IBBHQQ', img, st)
        if not nm_off or not shndx or not value:
            continue
        exports.append((name_at(nm_off), value))

    imports = []
    if 'JMPREL' in tags and 'PLTRELSZ' in tags:
        for off in range(0, tags['PLTRELSZ'], 24):
            r_off, r_info, r_add = struct.unpack_from('<QQq', img, tags['JMPREL'] + off)
            rtype = r_info & 0xFFFFFFFF
            symi = r_info >> 32
            st = symtab + symi * 24
            nm_off = struct.unpack_from('<I', img, st)[0]
            imports.append((name_at(nm_off), rtype))
    # 数据导入（GLOB_DAT 走 .rela.dyn，不在 JMPREL 里）
    if 'RELA' in tags and 'RELASZ' in tags:
        for off in range(0, tags['RELASZ'], 24):
            r_off, r_info, r_add = struct.unpack_from('<QQq', img, tags['RELA'] + off)
            rtype = r_info & 0xFFFFFFFF
            symi = r_info >> 32
            if rtype != 1025 or symi == 0:        # 1025 = R_AARCH64_GLOB_DAT
                continue
            st = symtab + symi * 24
            nm_off = struct.unpack_from('<I', img, st)[0]
            if nm_off:
                imports.append((name_at(nm_off), rtype))
    return exports, imports


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = sys.argv[1]
    out_json = None
    if '--json' in sys.argv:
        out_json = sys.argv[sys.argv.index('--json') + 1]

    order = ['rtld', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3', 'main']
    mods = {}
    for name in order:
        p = os.path.join(d, name)
        if not os.path.exists(p):
            print(f'  (缺 {name})')
            continue
        img = nso_to_image(p)
        tags = parse_dyn(img)
        ex, im = symbols(img, tags)
        ninit = tags.get('INIT_ARRAYSZ', 0) // 8
        mods[name] = {'size': len(img), 'exports': ex, 'imports': im, 'init_array': ninit,
                      'mod0_tags': {k: v for k, v in tags.items()}}
        print(f'{name:<9} 映像 {len(img)/1048576:6.2f} MB  导出 {len(ex):5d}  导入 {len(im):5d}  构造函数 {ninit:5d}')

    # 全局导出表（后面的模块可以解析前面所有模块的导出）
    gexp = {}
    for name in order:
        if name in mods:
            for nm, v in mods[name]['exports']:
                gexp.setdefault(nm, []).append((name, v))

    print('\n---- 跨模块解析 ----')
    unresolved = {}
    for name in order:
        if name not in mods:
            continue
        home = miss = 0
        miss_names = []
        for nm, rt in mods[name]['imports']:
            if nm in gexp:
                home += 1
            else:
                miss += 1
                miss_names.append(nm)
        print(f'{name:<9} 导入 {len(mods[name]["imports"]):5d} 条：{home:5d} 条能在已加载模块里解析，'
              f'{miss:5d} 条要宿主桩')
        if miss_names:
            unresolved[name] = sorted(set(miss_names))

    print('\n---- 各模块缺宿主桩最多的符号（前 12）----')
    for name, names in unresolved.items():
        print(f'  [{name}] {len(names)} 个不同符号，例：{", ".join(names[:12])}')

    if out_json:
        dump = {n: {'size': m['size'], 'n_exports': len(m['exports']), 'n_imports': len(m['imports']),
                    'init_array': m['init_array'],
                    'exports': [x[0] for x in m['exports']][:4000],
                    'imports': [x[0] for x in m['imports']][:4000]}
                for n, m in mods.items()}
        dump['_unresolved'] = {k: v[:400] for k, v in unresolved.items()}
        json.dump(dump, open(out_json, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
        print(f'\n✓ JSON 写到 {out_json}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
