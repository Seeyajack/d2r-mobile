#!/usr/bin/env python3
"""dm_trace.bin 离线分析器（配 dm/ohos/tools/run_probe.ps1 用）。

为什么要有它（2026-10-19 复盘）：
  一轮上机 7.4 分钟，机器时间只占 1.6 分钟——真正的浪费是"一次只问一个是非题"。
  现在探针把**每一次导入调用和关键站点命中**都写进环缓冲（一条 ~50 ns），跑完落成
  dm_trace.bin；分析全部搬到离线，上机次数直接降一个数量级。

用法：
  python trace_view.py <out目录>                # 摘要：总量/线程分布/Top 调用/首尾时间线
  python trace_view.py <out目录> --tail 40      # 只看最后 40 条（阻塞/崩溃前发生了什么）
  python trace_view.py <out目录> --top 40
  python trace_view.py <out目录> --tid 123
  python trace_view.py <out目录> --dump-csv x.csv
"""
import argparse
import os
import struct
import sys
from collections import Counter

MAGIC = 0x45434152544B4441          # "ADKT RACE"（小端读出来是这个数）
PLT_MARK = 0x10000000
ENT = struct.Struct("<IIQQ")        # off, tid, a0, a1


def load(out_dir):
    p = os.path.join(out_dir, "dm_trace.bin")
    if not os.path.exists(p):
        sys.exit(f"没有 {p}——本轮可能没跑到落盘点（脚本会提示）。")
    raw = open(p, "rb").read()
    if len(raw) < 32:
        sys.exit("文件太短，头部都没写完。")
    magic, n, lost, base = struct.unpack_from("<QQQQ", raw, 0)
    if magic != MAGIC:
        sys.exit(f"魔数不对（{magic:#x}）——不是本工具认识的文件。")
    body = raw[32:]
    n = min(n, len(body) // ENT.size)
    ents = [ENT.unpack_from(body, i * ENT.size) for i in range(n)]
    return ents, n, lost, base


def load_names(out_dir):
    p = os.path.join(out_dir, "dm_plt_names.txt")
    names = {}
    if os.path.exists(p):
        for line in open(p, encoding="utf-8", errors="replace"):
            idx, _, nm = line.rstrip("\n").partition("\t")
            if idx.isdigit():
                names[int(idx)] = nm
    return names


def label(off, names):
    if off & PLT_MARK:
        idx = off & 0xFFFFFF
        if idx == 0xFFFFFF:
            return "PLT?<未知槽>"
        return names.get(idx, f"PLT#{idx}")
    return f"+0x{off:x}"


def demangle(nm):
    """Itanium 名字太长，摘要里只留人看得懂的部分（够定位就行）。"""
    try:
        return nm.split("E", 1)[0] if nm.startswith("_Z") else nm
    except Exception:
        return nm


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", help="run_probe.ps1 的产物目录（含 dm_trace.bin）")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--tail", type=int, default=30)
    ap.add_argument("--head", type=int, default=15)
    ap.add_argument("--tid", type=int, default=0, help="只看某个线程（0=全部）")
    ap.add_argument("--dump-csv", default="")
    args = ap.parse_args()

    ents, n, lost, base = load(args.out_dir)
    names = load_names(args.out_dir)

    if args.tid:
        ents = [e for e in ents if e[1] == args.tid]
    show = ents
    print(f"基址 {base:#x}   条数 {n}   丢失 {lost}   过滤后 {len(show)}")

    if not show:
        return

    # 线程分布
    tids = Counter(e[1] for e in show)
    print("\n线程分布（tid : 条数）：")
    for tid, c in tids.most_common(8):
        print(f"  {tid:#010x}  {c}")

    # 调用排行
    cnt = Counter(label(e[0], names) for e in show)
    print(f"\nTop {args.top}（站点/导入 : 次数）：")
    for nm, c in cnt.most_common(args.top):
        print(f"  {c:>7}  {demangle(nm)}")

    # 时间线
    def line(e, i):
        off, tid, a0, a1 = e
        return f"  #{i:<6} tid={tid:#010x} {label(off, names):<42} a0={a0:#x} a1={a1:#x}"

    print(f"\n开头 {args.head} 条：")
    for i, e in enumerate(show[:args.head]):
        print(line(e, i))
    print(f"\n最后 {args.tail} 条（阻塞/崩溃前）：")
    for i, e in enumerate(show[-args.tail:], start=len(show) - min(args.tail, len(show))):
        print(line(e, i))

    if args.dump_csv:
        with open(args.dump_csv, "w", encoding="utf-8", newline="") as fp:
            fp.write("idx,tid,label,a0,a1\n")
            for i, e in enumerate(show):
                fp.write(f"{i},{e[1]:#x},{label(e[0], names)},{e[2]:#x},{e[3]:#x}\n")
        print(f"\n已导出 {args.dump_csv}")


if __name__ == "__main__":
    main()
