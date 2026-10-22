#!/usr/bin/env python3
"""dmport —— 一行命令：Switch 游戏 → 鸿蒙 HAP（工具链编排入口）

目标契约（路线图 M5）：
    dmport <游戏.xci 或 exefs 目录> -o <输出目录> [选项]

做的事（每一步都有判据，失败就停在那一步并打印原因）：
    ① 抽取    XCI → exefs（rtld / sdk / subsdk0-3 / main）
    ② 改写    每个模块 → <out>/rewritten/<名>.patched.bin + fixups 表（改写器 = patch_nso.py）
    ③ 勘察    7 个模块的导出/导入/构造函数 + 跨模块解析率（nso_survey.py）→ <out>/survey.json
    ④ 嵌入    生成 dm_images.S + dm_main_layout.h（gen_embed_ohos.py）
    ⑤ 构建    hvigor assembleHap → <out>/<名>.hap

为什么要有它：现在这五段是**手工串**的（本轮之前每一步都在命令行里手敲），换游戏就要重敲一遍，
而且容易漏步（比如忘了重生成嵌入、忘了 --layout-header）。把它固化成一条命令，
"通用化"才落到可执行的东西上。

零依赖：只用标准库 + 仓库内 dm/tools 的纯 Python 工具（lz4lite 已替掉 python-lz4）。

用法示例：
    # 全流程（有 XCI）
    python dm/ohos/tools/dmport.py "F:/switch-work/botw.xci" -o F:/switch-work/port
    # 已经有 exefs，只做改写→勘察→嵌入→构建
    python dm/ohos/tools/dmport.py --exefs F:/switch-work/botw/exefs -o F:/switch-work/port
    # 只跑到嵌入（不构建，适合离线核对）
    python dm/ohos/tools/dmport.py --exefs ... -o ... --skip-build
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))     # d2r-mobile/（HERE=dm/ohos/tools）
APP = os.path.join(REPO, 'dm', 'ohos', 'app')
MODULES = ['main', 'sdk', 'rtld', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3']
DEVECO = r'D:/Program Files/Huawei/DevEco Studio'


def log(step, msg):
    print(f'[{time.strftime("%H:%M:%S")}] {step:<6} {msg}', flush=True)


def sh(cmd, env=None, cwd=None, quiet=False):
    """跑子进程；失败立刻抛（带 stderr 尾部），不吞错误。"""
    e = dict(os.environ)
    if env:
        e.update(env)
    r = subprocess.run(cmd, env=e, cwd=cwd, capture_output=True, text=True)
    if r.returncode != 0:
        tail = (r.stderr or r.stdout or '')[-1500:]
        raise RuntimeError(f'命令失败（{r.returncode}）：{" ".join(cmd)}\n{tail}')
    if not quiet and r.stdout:
        print(r.stdout.rstrip()[-800:])
    return r


def step_extract(xci, out):
    """① XCI → exefs。产物目录 <out>/extract"""
    dst = os.path.join(out, 'extract')
    os.makedirs(dst, exist_ok=True)
    log('①抽取', f'{os.path.basename(xci)} → {dst}')
    sh([sys.executable, os.path.join(HERE, 'xci_extract.py'), xci, dst])
    # 找 exefs：抽取器把 NCA 内容按名字落盘，找含 main/rtld 的目录
    for root, _dirs, files in os.walk(dst):
        if 'main' in files and 'rtld' in files:
            log('①抽取', f'✓ 找到 exefs：{root}（{len(files)} 个文件）')
            return root
    raise RuntimeError(f'抽取完成但没找到 exefs（需要同时含 main 与 rtld）：{dst}')


def step_rewrite(exefs, out):
    """② 每个模块 → rewritten/<名>.patched.bin + .fixups.tsv"""
    dst = os.path.join(out, 'rewritten')
    os.makedirs(dst, exist_ok=True)
    ok = []
    for m in MODULES:
        src = os.path.join(exefs, m)
        if not os.path.exists(src):
            log('②改写', f'⚠ 缺模块 {m}（跳过——有的游戏没有 subsdk*）')
            continue
        log('②改写', f'{m} …')
        sh([sys.executable, os.path.join(REPO, 'dm', 'tools', 'patch_nso.py'), src, dst, m],
           env={'DM_PATCH_TARGET': 'ohos'}, quiet=True)
        binp = os.path.join(dst, f'{m}.patched.bin')
        if not os.path.exists(binp):
            raise RuntimeError(f'{m} 改写后没有产物 {binp}')
        ok.append(m)
        log('②改写', f'  ✓ {m}: {os.path.getsize(binp)/1048576:.2f} MB')
    if 'main' not in ok:
        raise RuntimeError('没有 main 模块，无法继续')
    return dst, ok


def step_survey(exefs, out, mods):
    """③ 勘察：导出/导入/构造函数 + 跨模块解析率"""
    js = os.path.join(out, 'survey.json')
    log('③勘察', f'7 模块地图 → {js}')
    r = sh([sys.executable, os.path.join(REPO, 'dm', 'tools', 'nso_survey.py'), exefs, '--json', js],
           quiet=True)
    print(r.stdout.rstrip())
    return js


def step_embed(rewritten, out, mods):
    """④ 生成 dm_images.S + dm_main_layout.h（写进仓库，因为构建要引用它们）"""
    asm = os.path.join(REPO, 'dm', 'ohos', 'dm_images.S')
    hdr = os.path.join(REPO, 'dm', 'ohos', 'dm_main_layout.h')
    log('④嵌入', f'{len(mods)} 个模块 → {os.path.relpath(asm, REPO)} + {os.path.relpath(hdr, REPO)}')
    # 备份旧的（构建失败可回滚；也是判据：能看出这轮换了什么）
    for p in (asm, hdr):
        if os.path.exists(p):
            shutil.copy2(p, os.path.join(out, os.path.basename(p) + '.prev'))
    sh([sys.executable, os.path.join(HERE, 'gen_embed_ohos.py'), '--layout-header', hdr,
        rewritten, asm] + mods)
    return asm, hdr


def step_build(out, name):
    """⑤ hvigor assembleHap（必须在能调 wmic 的环境里跑；沙箱会拦）"""
    log('⑤构建', 'hvigor assembleHap …（几分钟）')
    node = os.path.join(DEVECO, 'tools', 'node', 'node.exe')
    hv = os.path.join(DEVECO, 'tools', 'hvigor', 'bin', 'hvigorw.js')
    env = {
        'DEVECO_SDK_HOME': os.path.join(DEVECO, 'sdk'),
        'JAVA_HOME': os.path.join(DEVECO, 'jbr'),
        'PATH': os.path.join(DEVECO, 'jbr', 'bin') + os.pathsep + os.environ.get('PATH', ''),
    }
    # ⚠ hvigor 收尾会调 wmic.exe：在受限环境（沙箱/黑名单）里它被拦，进程返回非 0，
    # 但**编译本身是成功的**。判据不看退出码，看 HAP 有没有出来（出来了就算成功，并如实标注）。
    try:
        sh([node, hv, '--mode', 'module', '-p', 'product=default', '-p', 'module=entry@default',
            '-p', 'buildMode=debug', 'assembleHap', '--no-daemon'], env=env, cwd=APP, quiet=True)
    except RuntimeError as e:
        hap0 = os.path.join(APP, 'entry', 'build', 'default', 'outputs', 'default',
                            'entry-default-signed.hap')
        if 'wmic' in str(e) and os.path.exists(hap0):
            print('  ⚠ hvigor 退出码非 0，但原因是收尾调 wmic.exe 被环境拦（编译已完成）——按"产物存在"判定成功')
        else:
            raise
    hap = os.path.join(APP, 'entry', 'build', 'default', 'outputs', 'default',
                       'entry-default-signed.hap')
    if not os.path.exists(hap):
        raise RuntimeError(f'构建没有产出 HAP：{hap}')
    dst = os.path.join(out, f'{name}.hap')
    shutil.copy2(hap, dst)
    log('⑤构建', f'✓ {dst}（{os.path.getsize(dst)/1048576:.1f} MB）')
    return dst


def main(argv):
    ap = argparse.ArgumentParser(description='一行命令：Switch 游戏 → 鸿蒙 HAP')
    ap.add_argument('xci', nargs='?', help='游戏 .xci（也可以用 --exefs 直接给模块目录）')
    ap.add_argument('--exefs', help='已有 exefs 目录（含 main/rtld/sdk/subsdk*）')
    ap.add_argument('-o', '--out', required=True, help='输出目录')
    ap.add_argument('--name', default=None, help='产物名（默认取输入文件名）')
    ap.add_argument('--skip-extract', action='store_true')
    ap.add_argument('--skip-rewrite', action='store_true', help='复用 <out>/rewritten')
    ap.add_argument('--rewritten', help='复用已有的改写产物目录（默认 <out>/rewritten）')
    ap.add_argument('--skip-build', action='store_true', help='只跑到嵌入（离线核对用）')
    a = ap.parse_args(argv[1:])

    if not a.xci and not a.exefs:
        ap.error('要么给 .xci，要么给 --exefs')
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    name = a.name or (os.path.splitext(os.path.basename(a.xci or a.exefs))[0])

    log('开始', f'输入={a.xci or a.exefs}  输出={out}  产物名={name}')
    t0 = time.time()

    # ① 抽取
    if a.exefs:
        exefs = os.path.abspath(a.exefs)
        log('①抽取', f'跳过（用现成的 exefs：{exefs}）')
    elif a.skip_extract:
        exefs = os.path.join(out, 'extract')
        log('①抽取', f'跳过（假定已有：{exefs}）')
    else:
        exefs = step_extract(os.path.abspath(a.xci), out)

    # ② 改写
    if a.skip_rewrite:
        rewritten = os.path.abspath(a.rewritten) if a.rewritten else os.path.join(out, 'rewritten')
        mods = [m for m in MODULES if os.path.exists(os.path.join(rewritten, f'{m}.patched.bin'))]
        log('②改写', f'跳过（复用 {rewritten}，有 {len(mods)} 个模块：{", ".join(mods)}）')
        if 'main' not in mods:
            raise RuntimeError(f'{rewritten} 里没有 main.patched.bin')
    else:
        rewritten, mods = step_rewrite(exefs, out)

    # ③ 勘察
    step_survey(exefs, out, mods)

    # ④ 嵌入。**注意**：这里的顺序只决定 `.so` 里段的排布，**不决定装载顺序**——
    # 装载顺序写在代码里（探针的 g_xmods：rtld → subsdk0-3，sdk 更早由 k_load_sdk 装）。
    # 保持与仓库现状一致（main, sdk, rtld, subsdk0-3），这样重跑不产生无谓 diff。
    order = [m for m in ['main', 'sdk', 'rtld', 'subsdk0', 'subsdk1', 'subsdk2', 'subsdk3'] if m in mods]
    asm, hdr = step_embed(rewritten, out, order)

    # ⑤ 构建
    hap = None
    if a.skip_build:
        log('⑤构建', '跳过（--skip-build）')
    else:
        hap = step_build(out, name)

    dt = time.time() - t0
    log('完成', f'耗时 {dt/60:.1f} 分钟')
    print('\n=== 产物 ===')
    for k, v in [('exefs', exefs), ('rewritten', rewritten), ('survey', os.path.join(out, 'survey.json')),
                 ('dm_images.S', asm), ('dm_main_layout.h', hdr), ('hap', hap or '（未构建）')]:
        print(f'  {k:<16} {v}')
    print('\n判据自查：')
    print(f'  模块数 = {len(mods)}（应为 7；缺 subsdk* 说明游戏不带那部分）')
    print(f'  嵌入顺序 = {order}（只影响 .so 段排布；装载顺序由代码里的 g_xmods 决定）')
    print('  下一步：装机 → aa start → 点「全链探针」→ 看 dm_run_test.log 的 [构造审计]/[服务]/nnMain')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
