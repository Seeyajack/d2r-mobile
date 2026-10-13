#!/usr/bin/env python3
# 生成"把宿主单测搬到鸿蒙上跑"所需的两份产物：
#   dm/ohos/host_tests_gen.c     —— 单测清单（外部声明 + 表），供 host_tests.c 顺序调用
#   dm/ohos/host_tests_gen.cmake —— 源码清单 + "把 main 改名"的编译定义 + 额外 include
# Generates the two artefacts needed to run the host unit tests on HarmonyOS: the test roster
# (host_tests_gen.c) and the source list / per-source compile definitions (host_tests_gen.cmake).
#
# **为什么用生成器而不是手抄**：`dm/host/tests/Makefile` 里每个单测的源码清单都写在它那条 clang
# 命令里，那是**上游的权威**。手抄 27 条必然抄错、而且上游一改就过期。生成器让 Makefile 继续当
# 唯一事实来源——**这也是"把别人的项目接进来"该有的姿势：能读原项目的构建定义，就不要另起一套。**
# Why generate instead of transcribing: the Makefile's clang lines are the upstream authority for each
# test's source list. Transcribing 27 of them by hand invites errors and goes stale the moment upstream
# changes. Generating keeps the Makefile as the single source of truth -- which is the right posture when
# adopting someone else's project: read their build definition instead of inventing a parallel one.
#
# 用法：python3 dm/ohos/tools/gen_host_tests.py
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # dm/ohos/tools
DM_OHOS = os.path.dirname(HERE)                            # dm/ohos
REPO = os.path.dirname(os.path.dirname(DM_OHOS))           # 仓库根
TESTS = os.path.join(REPO, 'dm', 'host', 'tests')
MAKEFILE = os.path.join(TESTS, 'Makefile')

# 不搬的单测，以及原因（都要写清楚，不能默默跳过）
SKIP = {
    't_condvar_old': '与 t_condvar 源码相同、只是多一个宏，符号会冲突，二选一',
    't_audio_spsc_tsan': '线程消毒器版本，鸿蒙侧没有对应运行时',
}


def parse_makefile():
    """返回 {测试名: {'srcs': [...], 'defs': [...], 'incs': [...]}}，源码已转成仓库相对路径。"""
    out = {}
    cur = None
    with open(MAKEFILE, encoding='utf-8') as f:
        for raw in f.read().split('\n'):
            if raw.startswith('$(B)/t_'):
                cur = raw.split(':')[0][len('$(B)/'):]
                continue
            if raw.startswith('\t') and 'clang' in raw and cur:
                toks = shlex.split(raw.strip())
                srcs, defs, incs = [], [], []
                for t in toks:
                    if t.endswith('.c'):
                        # clang 行里的路径相对 dm/host/tests
                        p = os.path.normpath(os.path.join(TESTS, t))
                        srcs.append(os.path.relpath(p, REPO).replace('\\', '/'))
                    elif t.startswith('-D'):
                        defs.append(t[2:])
                    elif t.startswith('-I'):
                        d = os.path.normpath(os.path.join(TESTS, t[2:]))
                        incs.append(os.path.relpath(d, REPO).replace('\\', '/'))
                out[cur] = {'srcs': srcs, 'defs': defs, 'incs': incs}
                cur = None
                continue
            if raw.strip() and not raw.startswith('\t') and not raw.startswith('#'):
                cur = None
    return out


def sym_of(name):
    """单测名 → 改名后的入口符号。取掉开头的 t_，免得出现 dm_t_t_xxx 这种双重前缀。"""
    base = name[2:] if name.startswith('t_') else name
    return 'dm_test_' + base + '_main'


def main():
    rules = parse_makefile()
    if not rules:
        print('解析不出任何单测规则，Makefile 结构可能变了', file=sys.stderr)
        return 1

    names = sorted(n for n in rules if n not in SKIP)

    # ---- 1) 清单 C 文件 ----
    gen_c = os.path.join(DM_OHOS, 'host_tests_gen.c')
    with open(gen_c, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从\n')
        f.write('// dm/host/tests/Makefile 生成。改单测清单请改 Makefile，再重跑生成器。\n')
        f.write('// GENERATED -- do not edit by hand. Produced by dm/ohos/tools/gen_host_tests.py from\n')
        f.write('// dm/host/tests/Makefile; edit the Makefile and re-run the generator instead.\n')
        f.write('#include "host_tests.h"\n\n')
        for n in names:
            f.write('int %s(void);\n' % sym_of(n))
        f.write('\nconst dm_test_ent dm_host_tests[] = {\n')
        for n in names:
            f.write('    { "%s", %s },\n' % (n, sym_of(n)))
        f.write('};\n')
        f.write('const int dm_host_tests_count = (int)(sizeof dm_host_tests / sizeof dm_host_tests[0]);\n')

    # ---- 2) CMake 片段 ----
    all_srcs = []
    for n in names:
        for s in rules[n]['srcs']:
            if s not in all_srcs:
                all_srcs.append(s)
    # 每个单测的入口要改名，否则 27 个 main 一起链会撞车
    test_srcs = {n: rules[n]['srcs'][0] for n in names}
    incs = sorted({i for n in names for i in rules[n]['incs']})
    defs = sorted({d for n in names for d in rules[n]['defs']} | {'DM_TEST_HOOKS'})

    gen_cmake = os.path.join(DM_OHOS, 'host_tests_gen.cmake')
    with open(gen_cmake, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 生成。\n')
        f.write('# GENERATED -- do not edit by hand.\n\n')
        f.write('# 单测源码（相对仓库根）\n')
        f.write('set(DM_HOST_TEST_SRCS\n')
        for s in all_srcs:
            f.write('    "${DM_DIR}/%s"\n' % s)
        f.write(')\n\n')
        f.write('# 单测自己的 include 目录（相对仓库根）\n')
        f.write('set(DM_HOST_TEST_INCS\n')
        for i in incs:
            f.write('    "${DM_DIR}/%s"\n' % i)
        f.write(')\n\n')
        f.write('# 单测需要的宏\n')
        f.write('set(DM_HOST_TEST_DEFS %s)\n\n' % ' '.join(defs))
        f.write('# 把每个单测的 main 改名，避免 27 个 main 撞车\n')
        for n in names:
            f.write('set_source_files_properties("${DM_DIR}/%s" PROPERTIES COMPILE_DEFINITIONS "main=%s")\n'
                    % (test_srcs[n], sym_of(n)))

    print('生成：')
    print('  %s   （%d 个单测）' % (os.path.relpath(gen_c, REPO), len(names)))
    print('  %s（%d 个源文件）' % (os.path.relpath(gen_cmake, REPO), len(all_srcs)))
    if SKIP:
        print('跳过（原因见脚本里的 SKIP）：')
        for k, v in SKIP.items():
            print('  %s —— %s' % (k, v))
    return 0


if __name__ == '__main__':
    sys.exit(main())
