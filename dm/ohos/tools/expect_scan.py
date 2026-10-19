#!/usr/bin/env python3
"""期望值反推扫描器（离线）：游戏到底希望每个导入返回什么？

为什么要有它（2026-10-19 复盘）：
  这一天的返工里有很大一类是同一形状——**我们给的返回值不满足游戏的期望**：
    · nvnDeviceGetInteger(dev,0) 给了 53，游戏 `cmp w8,#0x34` 要**恰好 52**；
    · nvnBootstrapLoader 返回 0，游戏 `cbz` 一查就放弃整个图形初始化；
    · memcpy 是空桩，游戏不管返回值，但结构体全成 0。
  这类问题**完全可以离线批量发现**：找到每个导入的调用点，看调用之后几条指令怎么检查返回值，
  就能反推"它期望什么"。不用一个一个上机试。

做法：
  1) 从 JMPREL 建「GOT 槽 → 符号名」表；
  2) 在 .text 里扫 `adrp xN,<GOT 页>` + `ldr xT,[xN,#off]`，槽地址对得上就算一次"引用"；
  3) 在引用之后找 `blr xT`（少数是 `bl` 直调），再从调用点往后看 12 条指令，提取对返回值
     （x0/w0，或先 mov 到别的寄存器）的比较：cmp / cbz / cbnz / tbz / tst / and+#imm；
  4) 输出一张表：符号 → 调用点数 → 见过的检查形态 → 反推出的期望。

用法：
  python expect_scan.py                          # 全表
  python expect_scan.py --only-stub              # 只看"当前还是空桩"的（默认就是）
  python expect_scan.py --grep nvn               # 只看名字含 nvn 的
  python expect_scan.py --out 报告.txt
"""
import argparse
import os
import struct
import sys
from collections import defaultdict

IMG_DEFAULT = r"F:/switch-work/botw/rewritten/main.patched.bin"

# 映像布局（与 dm/ohos/dm_main_layout.h 一致）
TEXT_END = 0x1633B30


def sext(v, b):
    return v - (1 << b) if v & (1 << (b - 1)) else v


class Image:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        self._parse_dyn()

    def u32(self, o):
        return struct.unpack_from("<I", self.d, o)[0]

    def u64(self, o):
        return struct.unpack_from("<Q", self.d, o)[0]

    def _parse_dyn(self):
        d = self.d
        mod0 = next(o for o in range(0, 0x100, 4) if self.u32(o) == 0x30444F4D)
        dyn = mod0 + self.u32(mod0 + 4)
        tags = {}
        for i in range(64):
            t, v = self.u64(dyn + i * 16), self.u64(dyn + i * 16 + 8)
            if t == 0:
                break
            tags.setdefault(t, v)
        self.symtab, self.strtab = tags[6], tags[5]
        self.jmprel, self.pltsz = tags[0x17], tags[2]
        # GOT 槽 → 名字
        self.got_name = {}
        self.name_got = defaultdict(list)
        for i in range(self.pltsz // 24):
            r_off = self.u64(self.jmprel + i * 24)
            symi = self.u64(self.jmprel + i * 24 + 8) >> 32
            n = self.u32(self.symtab + symi * 24)
            end = d.find(b"\0", self.strtab + n)
            nm = d[self.strtab + n:end].decode("utf-8", "replace")
            self.got_name[r_off] = nm
            self.name_got[nm].append(r_off)


def scan_stubs(img):
    """找出 PLT 桩，返回 {桩地址: 符号名}。

    形态（固定 4 条）：
        adrp x16, <GOT 页>
        ldr  x17, [x16, #off]
        add  x16, x16, #off
        br   x17
    ⚠ 第一版我按"GOT 槽被 adrp+ldr 引用"去找调用点，结果每个符号只找到 1 处——
      那 1 处正是 PLT 桩自己：**客体从不直接碰 GOT 槽，它 `bl <桩>`**。
    """
    stubs = {}
    data = img.d
    for a in range(0, TEXT_END, 4):
        w = struct.unpack_from("<I", data, a)[0]
        if (w & 0x9F000000) != 0x90000000 or (w & 31) != 16:
            continue
        w1 = struct.unpack_from("<I", data, a + 4)[0]
        if (w1 & 0xFFC00000) != 0xF9400000 or ((w1 >> 5) & 31) != 16 or (w1 & 31) != 17:
            continue
        w2 = struct.unpack_from("<I", data, a + 8)[0]
        if (w2 & 0xFFC003FF) != 0x91000210:          # add x16, x16, #imm
            continue
        w3 = struct.unpack_from("<I", data, a + 12)[0]
        if w3 != 0xD61F0220:                          # br x17
            continue
        immlo, immhi = (w >> 29) & 3, (w >> 5) & 0x7FFFF
        page = (a & ~0xFFF) + (sext((immhi << 2) | immlo, 21) << 12)
        got = page + ((w1 >> 10) & 0xFFF) * 8
        nm = img.got_name.get(got)
        if nm:
            stubs[a] = nm
    return stubs


def scan_calls(img, stubs):
    """扫所有 b/bl，落到桩上的就是一次调用。返回 {符号名: [调用点地址]}。"""
    calls = defaultdict(list)
    data = img.d
    for a in range(0, TEXT_END, 4):
        w = struct.unpack_from("<I", data, a)[0]
        if (w & 0xFC000000) not in (0x14000000, 0x94000000):
            continue
        tgt = a + sext(w & 0x03FFFFFF, 26) * 4
        nm = stubs.get(tgt)
        if nm:
            calls[nm].append(a)
    return calls


def checks_after(img, call_addr, span=14):
    """从调用点往后看，提取对返回值（x0/w0 或它被 mov 到的寄存器）的检查。"""
    data = img.d
    ret_regs = {0}
    out = []
    for j in range(1, span + 1):
        a = call_addr + j * 4
        w = struct.unpack_from("<I", data, a)[0]
        # mov xD, x0  (orr xD,xzr,x0 或 mov)
        if (w & 0xFFE0FFE0) == 0xAA0003E0:                     # mov xD, xM
            dm, sm = (w >> 0) & 31, (w >> 16) & 31
            if sm in ret_regs:
                ret_regs.add(dm)
            continue
        # cbz / cbnz （W 或 X）
        if (w & 0x7E000000) == 0x34000000:
            rt = w & 31
            if rt in ret_regs:
                kind = "cbnz" if (w >> 24) & 1 else "cbz"
                out.append(f"{(a + 4 + sext(((w >> 5) & 0x7FFFF) << 2, 23)):#x}:{kind}")
            continue
        # cmp wN/xN, #imm  (subs wzr, wN, #imm)
        if (w & 0x7F800000) == 0x71000000:
            rn = (w >> 5) & 31
            if rn in ret_regs:
                imm = (w >> 10) & 0xFFF
                if (w >> 22) & 1:
                    imm <<= 12
                out.append(f"cmp#{imm}")
            continue
        # tst wN, #imm (ands wzr, wN, #imm)
        if (w & 0x7F800000) == 0x72000000:
            rn = (w >> 5) & 31
            if rn in ret_regs:
                out.append(f"tst#{(w >> 10) & 0xFFF:#x}")
            continue
        if len(out) >= 3:
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=IMG_DEFAULT)
    ap.add_argument("--grep", default="")
    ap.add_argument("--all", action="store_true", help="连'已实现'的也列（默认只列空桩风险面）")
    ap.add_argument("--out", default="")
    ap.add_argument("--max-per-sym", type=int, default=6, help="每个符号最多看多少个调用点")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.exit(f"找不到映像：{args.img}")
    img = Image(args.img)
    stubs = scan_stubs(img)
    calls = scan_calls(img, stubs)
    print(f"# 桩 {len(stubs)} 个，有调用点的符号 {len(calls)} 个", file=sys.stderr)

    # 我们已实现的那一批（与 dm_probe.c 分流顺序一致，粗粒度即可——这里只用来排序/过滤）
    impl_markers = ("_ZN2nn2fs", "memcpy", "memset", "memmove", "memcmp", "str", "sin", "cos",
                    "sqrt", "pow", "atan", "acos", "tanf", "exp", "log", "fabs", "fmod",
                    "nvnBootstrapLoader", "time", "clock", "sscanf", "vsnprintf", "tolower",
                    "strtod", "strtoul", "memalign", "nvnDeviceGetInteger", "_ZN2nn2vi",
                    "_ZN2nn2oe", "_ZN2nn2os17ConvertToTimeSpan", "_ZN2nn2os11SleepThread",
                    "_ZN2nn4util9VSNPrintf", "_ZN2nn8settingseq")

    rows = []
    for nm, sites_all in calls.items():
        if args.grep and args.grep not in nm:
            continue
        is_stub = not any(m in nm for m in impl_markers)
        if not args.all and not is_stub:
            continue
        seen = []
        for call in sites_all[:args.max_per_sym]:
            seen.extend(checks_after(img, call))
        rows.append((nm, len(sites_all), len(sites_all), seen))

    rows.sort(key=lambda r: (-len(set(r[3])), -r[1]))

    # ---- 风险分类：调用后第一个检查是 cbz（=0 就走失败分支）的，最危险 ----
    risky = []
    for nm, sites, nref, seen in rows:
        if any(s.endswith(":cbz") for s in seen):
            risky.append(nm)

    def render(fh):
        fh.write(f"# 导入期望值反推（{len(rows)} 个符号有调用点）\n")
        fh.write(f"# 映像：{args.img}\n")
        fh.write("# 列：符号 | 看过的调用点 | 调用后对返回值的检查\n")
        fh.write("# 读法：cbz/cbnz = 只判真假；cmp#52 = 要求恰好 52；tst# = 位掩码\n\n")
        fh.write(f"## ⚠ 高风险：返回 0 会直接走失败分支（{len(risky)} 个）\n")
        fh.write("#   这些函数若还是空桩（返 0），调用方会**认为失败并改走别的路**（甚至原地放弃）。\n")
        fh.write("#   注意：其中有些是「返回 0 才是成功」的（如 fs 系列返回 0 表示成功）——判读时要看\n")
        fh.write("#   调用点后面是「跳过错分支」还是「进入成功分支」，别机械照搬。\n")
        for nm in risky:
            fh.write(f"  {nm}\n")
        fh.write("\n## 明细\n\n")
        for nm, sites, nref, seen in rows:
            uniq = []
            for s in seen:
                if s not in uniq:
                    uniq.append(s)
            tag = " ⚠" if nm in risky else ""
            fh.write(f"{nm}{tag}\n    调用点 {sites}   检查：{' '.join(uniq) if uniq else '（没检查 / 只当指针用）'}\n")

    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            render(fh)
        print(f"已写 {args.out}（{len(rows)} 个符号）")
    else:
        render(sys.stdout)


if __name__ == "__main__":
    main()
