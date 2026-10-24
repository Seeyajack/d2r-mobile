#!/usr/bin/env python3
# 解码 DT_RELA 里的非 RELATIVE 重定位：类型分布 + 符号名（看是否还有"没接的线"）。
import struct, collections, re, sys

src = sys.argv[1] if len(sys.argv) > 1 else r'F:/switch-work/botw/rewritten/main.patched.bin'
f = open(src, 'rb').read()
i = f.find(b'MOD0')
dyn = i + struct.unpack_from('<I', f, i + 4)[0]
d = {}
for k in range(0, 0x200, 0x10):
    tag, val = struct.unpack_from('<QQ', f, dyn + k)
    if tag == 0:
        break
    d.setdefault(tag, val)
SYMTAB, STRTAB = d[6], d[5]
RELA, RELASZ = d[7], d[8]

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

TYPES = {0: 'NONE', 257: 'ABS64', 258: 'ABS32', 1025: 'GLOB_DAT', 1026: 'JUMP_SLOT',
         1027: 'RELATIVE', 1028: 'TLS_TPREL64', 1032: 'TLSDESC', 1030: 'IRELATIVE'}
cnt = collections.Counter(); samples = {}
for k in range(0, RELASZ, 24):
    r_off, r_info, r_add = struct.unpack_from('<QQq', f, RELA + k)
    t = r_info & 0xffffffff
    if t == 1027:
        continue
    sym = r_info >> 32
    cnt[t] += 1
    samples.setdefault(t, (r_off, sym, r_add))
print('非 RELATIVE 重定位类型分布：')
for t, n in cnt.most_common():
    off, sym, add = samples[t]
    nm = sym_name(sym) if sym else '（无符号）'
    print('  类型 %d (%-10s) × %d   样例 槽 %#x 符号 %s 加数 %#x' % (t, TYPES.get(t, '?'), n, off, nm, add & 0xffffffffffffffff))
names = collections.Counter()
for k in range(0, RELASZ, 24):
    r_off, r_info, r_add = struct.unpack_from('<QQq', f, RELA + k)
    if (r_info & 0xffffffff) == 1027:
        continue
    sym = r_info >> 32
    if sym:
        names[demangle(sym_name(sym))] += 1
print()
print('涉及的符号（前 25）：')
for nm, n in names.most_common(25):
    print('  %-60s ×%d' % (nm, n))
