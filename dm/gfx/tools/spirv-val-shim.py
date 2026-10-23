#!/usr/bin/env python3
"""spirv-val 替身（np226，仅本机尖刺用）。

背景：MSYS2 的 `mingw-w64-ucrt-x86_64-spirv-tools` 包里 `spirv-opt.exe` 能用，但 `spirv-val.exe`
在本机加载失败（同目录、同 DLL 集合，退出码非 0 且无输出）。为了让门 2 的校验不被这个环境问题卡住，
这里用 **spirv-opt 内置的校验器**代替：`spirv-opt` 默认在优化前对输入跑同一个 validator
（其 `--skip-validation` 选项的存在即证明校验默认开着）。

调用约定与上游一致：`spirv-val-shim.py [--target-env vulkan1.1|--target-env=vulkan1.1] <file.spv>`
退出码：0 = 校验通过；非 0 = 不通过（诊断由 spirv-opt 打到 stderr）。
"""
import os
import subprocess
import sys
import tempfile

OPT = os.environ.get('SPIRV_OPT_REAL') or r'C:\Users\Administrator\dm-tools\spirv\bin\spirv-opt.exe'


def main(argv):
    args = argv[1:]
    env = 'vulkan1.1'
    files = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--target-env' and i + 1 < len(args):
            env = args[i + 1]
            i += 2
            continue
        if a.startswith('--target-env='):
            env = a.split('=', 1)[1]
            i += 1
            continue
        if a.startswith('-'):
            i += 1
            continue
        files.append(a)
        i += 1
    if not files:
        sys.stderr.write('spirv-val-shim: 没有输入文件\n')
        return 2
    spv = files[-1]
    # 让 spirv-opt 走一遍"读入 → 校验 → 无优化输出"：输出丢到临时文件，不污染上游产物。
    fd, tmp = tempfile.mkstemp(suffix='.spv')
    os.close(fd)
    try:
        r = subprocess.run([OPT, f'--target-env={env}', spv, '-o', tmp],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.stderr.write(r.stderr or '')
            return r.returncode
        return 0
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


if __name__ == '__main__':
    sys.exit(main(sys.argv))
