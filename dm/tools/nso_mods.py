#!/usr/bin/env python3
"""NSO 模块发现 + 规范排序（dmport / nso_survey / gen_embed 共用）

为什么要有它：模块清单以前在三个工具里各写死一份（dmport 的 MODULES、nso_survey 的
order、dm_probe.c 的 g_xmods），换一款游戏（缺 subsdk*、多 subsdk4、只有 main+sdk）
就会三处不一致——最严重的是 dm_probe.c 引用不存在的 DM_IMG_<名>_* 宏直接编译不过。
收敛到一处：**以 exefs 目录里实际有的 NSO 文件为准**，按规范排序。

排序 = 依赖顺序（与 dm_probe.c 的装载顺序一致，保持旷野之息零漂移）：
    main(0) → sdk(1) → rtld(2) → subsdk0..subsdkN(3+N) → 其它按名字(100+)

NSO 判定：文件头 4 字节 == b"NSO0"（exefs 里还有 main.npdm 之类的非模块文件，
按魔数自动排除——patch_nso.py 对非 NSO 会 assert，不能直接喂）。
"""
import os
import re

NSO_MAGIC = b'NSO0'

_SUB = re.compile(r'^subsdk(\d+)$')


def is_nso(path):
    """文件存在且头 4 字节是 NSO0。"""
    try:
        with open(path, 'rb') as f:
            return f.read(4) == NSO_MAGIC
    except OSError:
        return False


def order_key(name):
    """规范排序键：main/sdk/rtld 最前，subsdkN 按数字，其余按名字。"""
    if name == 'main':
        return (0, 0, '')
    if name == 'sdk':
        return (1, 0, '')
    if name == 'rtld':
        return (2, 0, '')
    m = _SUB.match(name)
    if m:
        return (3, int(m.group(1)), '')
    return (100, 0, name)


def discover(exefs_dir):
    """列出目录里全部 NSO 模块，按规范排序。只认文件（不递归）。"""
    if not os.path.isdir(exefs_dir):
        raise NotADirectoryError(f'不是目录：{exefs_dir}')
    names = [n for n in os.listdir(exefs_dir)
             if os.path.isfile(os.path.join(exefs_dir, n)) and is_nso(os.path.join(exefs_dir, n))]
    return sorted(names, key=order_key)


def extras(names):
    """除掉 main/sdk（这两个在探针里有各自的定制装载路径），返回剩余模块（保持顺序）。"""
    return [n for n in names if n not in ('main', 'sdk')]
