#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_nvn_stubs.py —— 生成探针用的「NVN 按名字记账桩」表（dm_nvn_stubs.inc）。

为什么要这个文件（Task #9，2026-10-17 深夜）
--------------------------------------------
之前 `nvnDeviceGetProcAddress` 对每个名字都返回**同一个通用桩**。后果：日志只能看到
「被取走 N 次」，看不出游戏**先撞哪一族、先撞哪个函数**。而 NVN 这一层有 523 个 API，
全部实现是不可能的，必须按「游戏真实调用顺序」排期。

做法：每个名字一个独立桩（普通 C 函数，体内 1 条原子计数 + 首调日志），
GetProcAddress 按名字发对应地址。返回值和之前完全一样（一律 0）——
**这纯属观测，不改任何语义**，所以不会引入新崩点。

名字清单的来源（重要纠正，np26/np27 真机）
------------------------------------------
一开始用的是「BotW 日志里抓到的 120 个名字」（nvn-api-botw.txt）。np26 真机发现
**清单外名字刷了 400 条还没完** —— 因为那份清单本来就是旧的日志截断产物（旧代码只记前
120 次 GetProcAddress）。真机的实际查询量约 636 次、去重后 ~520 个，正好等于仓库里
`dm/gfx/nvn/names.txt` 的 523 个 —— 那才是 NVN 的**完整 API 面**，也是本生成器的默认输入。

族划分也不是 8 族，按名字首词统计（523 个）：
    Command 146 / Texture 96 / Sampler 48 / Device 35 / Queue 34 / Window 24 /
    Multisample 23 / Memory 18 / Buffer 18 / Blend 15 / Depth 13 / Event 11 /
    Vertex 10 / Polygon 9 / Program 7 / Color 7 / Sync 6 / Channel 3
**CommandBuffer 一族就占 28%** —— 这条线（命令流翻译）比原先估计的更集中，也更重要。

为什么用 C 宏生成而不是照抄上游 nvn_thunks.S 的汇编 thunk
--------------------------------------------------------
上游那套（`_nvnth_N`：stadd 计数 + `br` 到真实实现）目标是「透传到真实现」，
要在 x16/x17（IP0/IP1）里递槽地址。我们这一轮不透传、只记账 + 打日志，
普通 C 函数就是 ABI 合法的调用（x0–x18 本来就可被调用方 clobber），
比手写汇编安全得多；代价只是每桩多几条指令（523 × ~16B ≈ 8KB）。

用法
----
    python dm/ohos/tools/gen_nvn_stubs.py             # 默认用仓库 dm/gfx/nvn/names.txt
    python dm/ohos/tools/gen_nvn_stubs.py --names X   # 指定别的清单（换游戏时用实测清单）
    python dm/ohos/tools/gen_nvn_stubs.py --check     # 只比对，不写文件
"""

import argparse
import os
import re
import sys

# 名字首词 → (族宏名, 族显示名)。顺序即枚举顺序，**改这里必须重新生成**（枚举也在 .inc 里）。
FAMILIES = [
    ("Command",     "NVN_FAM_COMMAND",     "CommandBuffer"),
    ("Texture",     "NVN_FAM_TEXTURE",     "Texture"),
    ("Sampler",     "NVN_FAM_SAMPLER",     "Sampler"),
    ("Device",      "NVN_FAM_DEVICE",      "Device"),
    ("Queue",       "NVN_FAM_QUEUE",       "Queue"),
    ("Window",      "NVN_FAM_WINDOW",      "Window"),
    ("Multisample", "NVN_FAM_MULTISAMPLE", "Multisample"),
    ("Memory",      "NVN_FAM_MEMPOOL",     "MemoryPool"),
    ("Buffer",      "NVN_FAM_BUFFER",      "Buffer"),
    ("Blend",       "NVN_FAM_BLEND",       "Blend"),
    ("Depth",       "NVN_FAM_DEPTH",       "Depth"),
    ("Event",       "NVN_FAM_EVENT",       "Event"),
    ("Vertex",      "NVN_FAM_VERTEX",      "Vertex"),
    ("Polygon",     "NVN_FAM_POLYGON",     "Polygon"),
    ("Program",     "NVN_FAM_PROGRAM",     "Program"),
    ("Color",       "NVN_FAM_COLOR",       "Color"),
    ("Sync",        "NVN_FAM_SYNC",        "Sync"),
    ("Channel",     "NVN_FAM_CHANNEL",     "Channel"),
]
WORD_RE = re.compile(r"^nvn([A-Z][a-z]+)")
BY_STEM = {stem: (macro, disp) for stem, macro, disp in FAMILIES}


def classify(name):
    m = WORD_RE.match(name)
    if not m:
        return None
    return BY_STEM.get(m.group(1))


def read_names(path):
    names = []
    with open(path, "r", encoding="utf-8", newline=None) as f:
        for raw in f:
            s = raw.strip().rstrip(")").strip()   # 早期抓到的一批行尾多一个 ')'（正则残留）
            if not s or s.startswith("#"):
                continue
            if not re.fullmatch(r"nvn[A-Za-z0-9_]+", s):
                raise SystemExit("非法名字：%r" % s)
            names.append(s)
    dup = sorted({x for x in names if names.count(x) > 1})
    if dup:
        raise SystemExit("重复名字：%s" % dup)
    return names


def render(names, src):
    o = []
    o.append("// ⚠ 生成文件，请勿手改。生成器：dm/ohos/tools/gen_nvn_stubs.py\n")
    o.append("// 输入：%s（%d 个名字 = NVN 完整 API 面）\n" % (src, len(names)))
    o.append("//\n")
    o.append("// 内容：每名字一个「记账桩」+ 族枚举 + 名字表 + 桩地址表。\n")
    o.append("// 用途：让 GetProcAddress 按名字发地址，从日志里看出游戏**先真正调用哪个族/哪个函数**\n")
    o.append("//       （NVN 共 %d 个 API，只能按真实调用顺序排期实现）。\n" % len(names))
    o.append("// 行为：桩一律 return 0 —— 与「全返同一个通用桩」完全等价，纯观测、不改语义。\n\n")

    # 1. 族枚举 + 显示名
    o.append("// ---- 1. 族枚举（顺序 = 生成器 FAMILIES，改顺序要重新生成）----\n")
    o.append("enum {\n")
    for _, macro, _ in FAMILIES:
        o.append("    %s,\n" % macro)
    o.append("    NVN_FAM__N\n};\n")
    o.append("static const char *const g_nvn_fam_name[NVN_FAM__N] = {\n")
    for _, _, disp in FAMILIES:
        o.append('    "%s",\n' % disp)
    o.append("};\n\n")

    # 2. 类型
    o.append("// ---- 2. 类型 ----\n")
    o.append("typedef struct { const char *name; int fam; uint64_t calls; } nvn_api_t;\n")
    o.append("typedef uint64_t (*nvn_thunk_fn)(void);\n\n")

    # 3. 名字表
    o.append("// ---- 3. 名字表（顺序 = 输入文件顺序，保持稳定便于 diff）----\n")
    o.append("static nvn_api_t g_nvn_api[] = {\n")
    for nm in names:
        fam = classify(nm)
        if not fam:
            raise SystemExit("无法归类：%s（请在 FAMILIES 里补首词）" % nm)
        o.append('    { "%s", %s },\n' % (nm, fam[0]))
    o.append("};\n#define NVN_API_N ((int)(sizeof g_nvn_api / sizeof g_nvn_api[0]))\n\n")

    # 4. 桩
    o.append("// ---- 4. 每名字一个记账桩（体内只有一次原子自增 + 首调/里程碑日志）----\n")
    o.append("#define NVN_TH_DEF(I) static uint64_t nvn_th_##I(void) { return nvn_api_touch(I); }\n")
    for i in range(len(names)):
        o.append("NVN_TH_DEF(%d)\n" % i)

    # 5. 桩地址表
    o.append("\n// ---- 5. 桩地址表（与 g_nvn_api 同下标）----\n")
    o.append("static const nvn_thunk_fn g_nvn_thunks[NVN_API_N] = {\n")
    for i in range(len(names)):
        o.append("    nvn_th_%d,\n" % i)
    o.append("};\n")
    return "".join(o)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--names", default=None, help="名字清单（默认仓库 dm/gfx/nvn/names.txt）")
    ap.add_argument("--out", default=None, help="输出（默认 cpp/dm_nvn_stubs.inc）")
    ap.add_argument("--check", action="store_true", help="只比对，不写")
    a = ap.parse_args()

    here = os.path.dirname(os.path.abspath(__file__))                 # dm/ohos/tools
    repo = os.path.abspath(os.path.join(here, "..", "..", ".."))      # d2r-mobile
    names_path = a.names or os.path.join(repo, "dm", "gfx", "nvn", "names.txt")
    out_path = a.out or os.path.join(
        repo, "dm", "ohos", "app", "entry", "src", "main", "cpp", "dm_nvn_stubs.inc")

    names = read_names(names_path)
    text = render(names, os.path.relpath(names_path, repo).replace("\\", "/"))

    if a.check:
        old = open(out_path, "r", encoding="utf-8").read() if os.path.exists(out_path) else ""
        print("一致：%d 个名字" % len(names) if old == text
              else "不一致：%s 需要重新生成" % out_path)
        return 0 if old == text else 1

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)

    from collections import Counter
    c = Counter(classify(n)[1] for n in names)
    print("已写出 %s" % out_path)
    print("共 %d 个名字：" % len(names))
    for disp, n in c.most_common():
        print("   %-14s %d" % (disp, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
