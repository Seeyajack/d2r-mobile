import io, re, sys

def fix(path):
    s = io.open(path, encoding='utf-8').read()
    n = 0
    # ① Mach-O 重定位后缀 → ELF：_sym@PAGE → sym；_sym@PAGEOFF → :lo12:sym
    def repl_pageoff(m):
        return 'add %s, %s, :lo12:%s' % (m.group(1), m.group(1), m.group(2))
    s, k = re.subn(r'add (x\d+), \1, _([A-Za-z0-9_]+)@PAGEOFF', repl_pageoff, s); n += k
    s, k = re.subn(r'adrp (x\d+), _([A-Za-z0-9_]+)@PAGE', r'adrp \1, \2', s); n += k
    # ② 前导下划线（Mach-O 符号风格）→ ELF 无前缀
    s, k = re.subn(r'\b_(nvn_[a-z0-9_]+|mach_absolute_time)\b', r'\1', s); n += k
    # ③ .cstring（Mach-O 的 C 字符串段）→ ELF 的 .rodata
    s, k = re.subn(r'^\s*\.cstring\s*$', r'.section .rodata', s, flags=re.M); n += k
    # ④ mach_absolute_time（苹果专有）→ 我们提供的 nvn_entry_now_ns()
    s, k = s.replace('bl mach_absolute_time', 'bl nvn_entry_now_ns'), s.count('bl mach_absolute_time')
    n += k
    io.open(path, 'w', encoding='utf-8').write(s)
    print('%s：改了 %d 处' % (path, n))

for p in sys.argv[1:]:
    fix(p)
