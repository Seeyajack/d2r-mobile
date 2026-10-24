#!/usr/bin/env python3
"""pack_romfs_subset.py —— 从 BotW 完整 romfs 里圈出「首启最小集」（np240，离线）

为什么需要：romfs 全量 14 GB，手机/HAP 都装不下；而首次起 guest 只需要其中一部分。
做法：按顶层目录白名单拷贝（保留目录结构——客体按 `/Event/xxx` 这类路径访问）。

用法：
  pack_romfs_subset.py <完整 romfs> <输出目录> [目录...]
  不给目录名时用内置的 DEFAULT（首启最小集）。

内置依据（自上而下越小越可能是启动必需）：
  System   1.4M   RegionLangMask / Version / Resource —— 启动就要读
  Font     3.1M   字体
  Layout   1.7M   界面布局
  Game      97M   Stats / FarModelCullMgr —— 游戏参数
  Effect    73M   特效（早期 UI/过场就要）
  Actor    147M   actor 参数
  UI       961M   界面（先不带，等日志说缺了再加——它最大）
"""
import os
import shutil
import sys

DEFAULT = ["System", "Font", "Layout", "Game", "Effect", "Actor"]

def main():
    src, dst = sys.argv[1], sys.argv[2]
    dirs = sys.argv[3:] or DEFAULT
    total = 0
    for d in dirs:
        s = os.path.join(src, d)
        if not os.path.isdir(s):
            print("跳过（不存在）：%s" % s)
            continue
        t = os.path.join(dst, d)
        print("拷贝 %s …" % d)
        shutil.copytree(s, t)
        n = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(t) for f in fs)
        total += n
        print("  %s：%.1f MB" % (d, n / 1048576))
    print("最小集合计 %.1f MB → %s" % (total / 1048576, dst))

main()
