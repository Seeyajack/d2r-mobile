#!/usr/bin/env python3
"""export_all.py —— 把 shader_map.tsv 里的**全部** shader 导成 .xgs（S3 素材，np234）

用法：export_all.py <map.tsv> <pkg 目录> <主 NSO> <输出目录> [每批键数] [从第几批开始]
分批的原因：dmsh_vk.py 的键是从命令行传的，17309 个参数会超命令行长度限制；
              每批单独跑一次，失败可单独重跑（断点续跑）。
"""
import os
import subprocess
import sys
import time

MAP, PKG, MAIN, OUT = sys.argv[1:5]
BATCH = int(sys.argv[5]) if len(sys.argv) > 5 else 300
START = int(sys.argv[6]) if len(sys.argv) > 6 else 0
END = int(sys.argv[7]) if len(sys.argv) > 7 else 10 ** 9   # 结束批号（不含），用于分段跑

KEYS = []
for ln in open(MAP, encoding='utf-8'):
    if ln.startswith('#') or ln.startswith('package\t'):
        continue
    f = ln.rstrip('\n').split('\t')
    # 列：package, index, stage, key, source（键是第 4 列，与 run_gate2.py 的 c[3] 一致）
    if len(f) >= 4 and f[2] in ('compute', 'pixel', 'vertex', 'geometry'):
        KEYS.append((f[3], f[2]))          # (key, stage)

print(f'共 {len(KEYS)} 个 shader，每批 {BATCH}，从第 {START} 批开始', flush=True)
os.makedirs(OUT, exist_ok=True)

HERE = os.path.dirname(os.path.abspath(__file__))
# dmsh_vk.py 与本驱动同在 dm/gfx/tools 下
DM = os.path.join(HERE, 'dmsh_vk.py')
if not os.path.exists(DM):
    sys.exit('找不到 dmsh_vk.py：' + DM)

chunks = [KEYS[i:i + BATCH] for i in range(0, len(KEYS), BATCH)]
t0 = time.time()
done = 0
for ci, ch in enumerate(chunks):
    if ci < START or ci >= END:
        done += len(ch)
        continue
    sub = os.path.join(OUT, 'batch%05d' % ci)
    os.makedirs(sub, exist_ok=True)
    cmd = [sys.executable, DM, PKG, sub] + [k for k, _ in ch]
    r = subprocess.run(cmd, capture_output=True, text=True)
    done += len(ch)
    tail = (r.stdout or '').strip().splitlines()[-3:]
    el = time.time() - t0
    print(f'[批 {ci + 1}/{len(chunks)}] 本批 {len(ch)} 个，累计 {done}/{len(KEYS)}，'
          f'用时 {el / 60:.1f} 分，剩约 {(len(KEYS) - done) * el / max(done, 1) / 60:.0f} 分',
          flush=True)
    for t in tail:
        print('    ' + t, flush=True)
print(f'全部跑完，用时 {(time.time() - t0) / 60:.1f} 分钟', flush=True)
