#!/usr/bin/env python3
"""NVN 覆盖率：游戏**要**哪些 NVN 函数 vs 我们**实现了**哪些（M4 的按需工作单）

为什么这么做（2026-10-22）：
  main 只导入一个 `nvnBootstrapLoader`——NVN 是**运行时按名字取函数表**的。所以"游戏用哪些 NVN 函数"
  不用猜：这些名字就躺在 main 的 `.rodata` 里（442 个）。把它和我们实现的集合做差集，
  就得到一张**按需求排序**的 M4 工作单——不用再凭感觉写（写完又可能没人用）。

用法：
  python dm/tools/nvn_coverage.py --game F:/switch-work/botw/rewritten/main.patched.bin [--md out.md]
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))

# 实现所在的文件（改这里就是改"--impl 的来源"）
IMPL_FILES = [
    'dm/ohos/dm_nvn_ohos.c',
    'dm/ohos/dm_nvn_vk.c',
    'dm/ohos/dm_nvn_state_ohos.c',   # M4-a：状态族 77 个
    'dm/gfx/nvn/nvn_vk.c',
    'dm/gfx/nvn/nvn_tex.c',
]


def game_names(path):
    img = open(path, 'rb').read()
    out = set()
    for m in re.finditer(rb'nvn[A-Za-z0-9_]{2,64}\x00', img):
        s = m.group(0)[:-1].decode('ascii', 'replace')
        if re.fullmatch(r'nvn[A-Z][A-Za-z0-9]*', s):
            out.add(s)
    return out


def impl_names():
    """从实现文件里抽 "nvnXxx" 字符串字面量（我们按名字分派的表就是这么写的）"""
    out = {}
    for rel in IMPL_FILES:
        p = os.path.join(REPO, rel)
        if not os.path.exists(p):
            continue
        src = open(p, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'"(nvn[A-Za-z0-9_]+)"', src):
            out.setdefault(m.group(1), set()).add(rel)
    return out


# 按对象族分组：M4 的施工本来就按对象做（Window / Queue / CommandBuffer / Texture …）
FAMILIES = [
    ('Window', ('nvnWindow',)),
    ('Queue', ('nvnQueue',)),
    ('CommandBuffer', ('nvnCommandBuffer',)),
    ('Texture', ('nvnTexture',)),
    ('Sampler', ('nvnSampler',)),
    ('Program/Shader', ('nvnProgram', 'nvnShader', 'nvnShaderData')),
    ('Buffer/Memory', ('nvnBuffer', 'nvnMemoryPool', 'nvnSync', 'nvnFence')),
    ('Pipeline/States', ('nvnBlendState', 'nvnChannelMaskState', 'nvnColorState', 'nvnDepthStencilState',
                         'nvnMultisampleState', 'nvnPolygonState', 'nvnRasterizerState',
                         'nvnVertexAttribState', 'nvnVertexStreamState', 'nvnViewport', 'nvnScissor')),
    ('Device/Other', ('nvnDevice', 'nvnBootstrap', 'nvnEvent', 'nvnGpu', 'nvnSemaphore')),
]


def family_of(name):
    for fam, prefixes in FAMILIES:
        if name.startswith(prefixes):
            return fam
    return '其他'


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True, help='改写后的 main 映像（flat）')
    ap.add_argument('--md', help='把报告写成 Markdown')
    a = ap.parse_args(argv[1:])

    need = game_names(a.game)
    have = impl_names()
    missing = sorted(n for n in need if n not in have)
    covered = sorted(n for n in need if n in have)
    extra = sorted(n for n in have if n not in need)

    lines = []
    def emit(s=''):
        print(s)
        lines.append(s)

    emit(f'=== NVN 覆盖率（游戏需求 vs 我方实现）===')
    emit(f'  游戏请求 {len(need)} 个 · 已实现 {len(covered)} 个 · **缺 {len(missing)} 个**'
         f'（覆盖率 {len(covered)/max(1,len(need))*100:.1f}%）')
    emit(f'  我方额外实现（游戏没要的）{len(extra)} 个 —— 这些是"多写的"，可留作换游戏用')
    emit('')
    emit('| 对象族 | 需求 | 已实现 | 缺 | 缺口明细（前 8） |')
    emit('|---|---|---|---|---|')
    fams = {}
    for n in need:
        fams.setdefault(family_of(n), []).append(n)
    for fam, _pref in FAMILIES + [('其他', ())]:
        ns = sorted(fams.get(fam, []))
        if not ns:
            continue
        miss = [n for n in ns if n not in have]
        hit = [n for n in ns if n in have]
        sample = ', '.join(m[4:] for m in miss[:8]) if miss else '—'
        emit(f'| {fam} | {len(ns)} | {len(hit)} | **{len(miss)}** | {sample} |')

    if missing:
        emit('')
        emit('### 缺口全量（按对象族）')
        for fam, _pref in FAMILIES + [('其他', ())]:
            ns = sorted(n for n in fams.get(fam, []) if n not in have)
            if ns:
                emit(f'- **{fam}**（{len(ns)}）：' + ', '.join(ns))

    if a.md:
        open(a.md, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines) + '\n')
        print(f'\n✓ 报告写到 {a.md}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
