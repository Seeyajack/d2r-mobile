#!/usr/bin/env python3
# 生成「模块外部依赖清单」：读模块自己的动态段（DT_JMPREL/DT_SYMTAB/DT_STRTAB），
# 逐条解出 PLT 重定位的符号名与槽地址，输出 Markdown 清单。
# 用法：python gen_imports_list.py <patched.bin> [输出.md]
import struct, collections, re, sys, io

src = sys.argv[1] if len(sys.argv) > 1 else r'F:/switch-work/botw/rewritten/main.patched.bin'
dst = sys.argv[2] if len(sys.argv) > 2 else '导入清单-旷野之息main.md'
f = open(src, 'rb').read()

# 从 MOD0 动态段取表位置（不写死 0x1b5xxxx，随构建而变）
i = f.find(b'MOD0')
assert i >= 0, '未找到 MOD0'
dyn = i + struct.unpack_from('<I', f, i + 4)[0]
d = {}
for k in range(0, 0x200, 0x10):
    tag, val = struct.unpack_from('<QQ', f, dyn + k)
    if tag == 0:
        break
    d.setdefault(tag, val)
SYMTAB = d[0x6]; STRTAB = d[0x5]; JMPREL = d[0x17]; PLTRELSZ = d[0x2]
RELA = d.get(0x7, 0); RELASZ = d.get(0x8, 0); RELACOUNT = d.get(0x6ffffff9, 0)

def sym_name(idx):
    off = struct.unpack_from('<I', f, SYMTAB + idx * 24)[0]
    e = f.find(b'\0', STRTAB + off)
    return f[STRTAB + off:e].decode('utf-8', 'replace')

def demangle(n):
    if not n.startswith('_ZN'):
        return n
    s = n[3:]; parts = []; i2 = 0
    while i2 < len(s):
        m = re.match(r'(\d+)', s[i2:])
        if not m:
            break
        ln = int(m.group(1)); i2 += len(m.group(1))
        parts.append(s[i2:i2 + ln]); i2 += ln
        if i2 < len(s) and s[i2] == 'E':
            break
    return '::'.join(parts) if parts else n

def domain(n):
    dm = demangle(n)
    if '::' in dm:
        return '::'.join(dm.split('::')[:2])
    return dm[:30]

rows = []
for k in range(0, PLTRELSZ, 24):
    r_off, r_info, r_add = struct.unpack_from('<QQq', f, JMPREL + k)
    n = sym_name(r_info >> 32)
    rows.append((r_off, n, demangle(n)))
rows.sort()
cat = collections.Counter(domain(n) for _, n, _ in rows)

L = []
L.append('# 旷野之息 main 模块的外部依赖清单（%d 项，PLT 重定位表全量）' % len(rows))
L.append('')
L.append('> 生成：`tools/ohos/gen_imports_list.py`（读模块自己的动态段 DT_JMPREL/DT_SYMTAB/DT_STRTAB）。')
L.append('> 这些槽在数据段，等着宿主填：**先打记录型存根（跑起来看要什么），再按崩点逐个实现**。')
L.append('')
L.append('## 重定位总览')
L.append('')
L.append('| 表 | 偏移 | 大小 | 条数 |')
L.append('|---|---|---|---|')
L.append('| RELATIVE（内部指针，机械可全量应用） | `%#x` | `%#x` | %d（DT_RELACOUNT） |' % (RELA, RELASZ, RELACOUNT))
L.append('| PLT（外部 SDK 符号，需填槽） | `%#x` | `%#x` | %d |' % (JMPREL, PLTRELSZ, PLTRELSZ // 24))
L.append('')
L.append('## 按符号域统计')
L.append('')
L.append('| 域 | 项数 |')
L.append('|---|---|')
for c, n in cat.most_common():
    L.append('| %s | %d |' % (c, n))
L.append('')
L.append('## 全清单（槽地址 — 符号 — 解名）')
L.append('')
L.append('| 槽 | 符号 | 解名 |')
L.append('|---|---|---|')
for off, n, dm in rows:
    L.append('| %#x | %s | %s |' % (off, n, dm))
L.append('')
io.open(dst, 'w', encoding='utf-8', newline='\n').write('\n'.join(L))
print('已写入', dst, '共', len(rows), '项；域分布：', dict(cat.most_common(8)))
