#!/usr/bin/env python3
# 生成"把宿主单测搬到鸿蒙上跑"所需的两份产物：
#   dm/ohos/host_tests_gen.c     —— 单测清单（供跑测框架 dlopen 用）
#   dm/ohos/host_tests_gen.cmake —— 每个单测一个 .so 的构建定义
# Generates the two artefacts needed to run the host unit tests on HarmonyOS: the roster (used by the
# runner to dlopen each test) and the per-test shared-library build definitions.
#
# **为什么用生成器而不是手抄**：`dm/host/tests/Makefile` 里每个单测的源码清单都写在它那条 clang
# 命令里，那是**上游的权威**。手抄 27 条必然抄错、而且上游一改就过期。生成器让 Makefile 继续当
# 唯一事实来源——**这也是"把别人的项目接进来"该有的姿势：能读原项目的构建定义，就不要另起一套。**
# Why generate instead of transcribing: the Makefile's clang lines are the upstream authority for each
# test's source list. Transcribing 27 by hand invites errors and goes stale the moment upstream changes.
# Generating keeps the Makefile as the single source of truth.
#
# **为什么"每个单测一个 .so"**：上游在 macOS 上靠 `-dead_strip` 把"这个测试没调到的函数，连同它们
# 对别处的引用一起丢掉"，所以只链一部分宿主也没问题。ELF 上 `-shared` 会导出全部符号，
# `--gc-sections` 也丢不掉 ⇒ 把 27 个合成一个会得到 170 个未解析符号（实测）。
# 每个单测单独一个 .so 则 26/27 直接通过。**照上游的结构走，别硬塞。**
# Why one .so per test: upstream relies on macOS's -dead_strip to drop functions the test never calls
# together with their external references, so linking only part of the host works there. On ELF, -shared
# exports everything and --gc-sections cannot drop it, so merging all 27 into one yields 170 unresolved
# symbols (measured). One .so per test links 26/27 straight away.
#
# 用法：python3 dm/ohos/tools/gen_host_tests.py
import os
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # dm/ohos/tools
DM_OHOS = os.path.dirname(HERE)                            # dm/ohos
REPO = os.path.dirname(os.path.dirname(DM_OHOS))           # 仓库根
TESTS = os.path.join(REPO, 'dm', 'host', 'tests')
MAKEFILE = os.path.join(TESTS, 'Makefile')

# 不搬的单测，以及原因（都要写清楚，不能默默跳过）
SKIP = {
    't_condvar_old': '与 t_condvar 源码相同、只是多一个宏（-DDM_TEST_CV_OLD_ORDER），二选一',
    't_audio_spsc_tsan': '线程消毒器版本，鸿蒙侧没有对应运行时',
    't_rift_clock': '上游这条规则靠 macOS 的 -dead_strip 把"测试没调到的宿主函数连同它们的引用'
                    '一起丢掉"，所以只链 hle_os.c 就够；ELF 的 -shared 丢不掉，会缺 16 个宿主符号'
                    '（dm_str / dm_game_notice / g_nmod / dm_hud_request_players …）。'
                    '要搬它得先把那些宿主文件补进它的清单，或换一种链接策略——留作待办。',
}

# 每个单测 .so 都要带上的支撑源（相对仓库根）：垫片层 + 我们自己的日志/音频后端。
# 单测在鸿蒙侧会引用 dm_log_printf 与 dm_ao_*，所以要一起链进去——每个 .so 自成一体。
SUPPORT_EXTRA = ['dm/ohos/dm_log.c', 'dm/ohos/audio_ohos.c']


def parse_makefile():
    """返回 {测试名: {'srcs': [...], 'defs': [...], 'incs': [...]}}，路径已转成仓库相对。"""
    out = {}
    cur = None
    with open(MAKEFILE, encoding='utf-8') as f:
        for raw in f.read().split('\n'):
            if raw.startswith('$(B)/t_'):
                cur = raw.split(':')[0][len('$(B)/'):]
                continue
            if raw.startswith('\t') and 'clang' in raw and cur:
                srcs, defs, incs = [], [], []
                for t in shlex.split(raw.strip()):
                    if t.endswith('.c'):
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


def main():
    rules = parse_makefile()
    if not rules:
        print('解析不出任何单测规则，Makefile 结构可能变了', file=sys.stderr)
        return 1

    names = sorted(n for n in rules if n not in SKIP)
    compat = sorted('dm/ohos/compat/src/' + f
                    for f in os.listdir(os.path.join(DM_OHOS, 'compat', 'src'))
                    if f.endswith('.c'))

    # ---- 1) 清单 C 文件（跑测框架靠它知道该 dlopen 哪些 .so）----
    gen_c = os.path.join(DM_OHOS, 'host_tests_gen.c')
    with open(gen_c, 'w', encoding='utf-8', newline='\n') as f:
        f.write('// 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从\n')
        f.write('// dm/host/tests/Makefile 生成。改单测清单请改 Makefile，再重跑生成器。\n')
        f.write('// GENERATED -- do not edit by hand. Produced by dm/ohos/tools/gen_host_tests.py from\n')
        f.write('// dm/host/tests/Makefile; edit the Makefile and re-run the generator instead.\n')
        f.write('#include "host_tests.h"\n\n')
        f.write('// lib 是不带前缀与后缀的 .so 名（与 cmake 里 OUTPUT_NAME 一致），dlopen 时拼上目录与 .so\n')
        f.write('const dm_test_ent dm_host_tests[] = {\n')
        for n in names:
            f.write('    { "%s", "dmt_%s" },\n' % (n, n))
        f.write('};\n')
        f.write('const int dm_host_tests_count = (int)(sizeof dm_host_tests / sizeof dm_host_tests[0]);\n')

    # ---- 2) CMake 片段 ----
    incs = sorted({i for n in names for i in rules[n]['incs']} | {'dm/host'})
    defs = sorted({d for n in names for d in rules[n]['defs']})
    gen_cmake = os.path.join(DM_OHOS, 'host_tests_gen.cmake')
    with open(gen_cmake, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# 【生成文件，不要手改】由 dm/ohos/tools/gen_host_tests.py 从\n')
        f.write('# dm/host/tests/Makefile 生成。改单测请改 Makefile，再重跑生成器。\n')
        f.write('# GENERATED -- do not edit by hand.\n')
        f.write('#\n')
        f.write('# 结构照上游"每个单测一个可执行文件"——在鸿蒙侧对应"每个单测一个 .so"。\n')
        f.write('# 合成一个会得到大量未解析符号（ELF 的 -shared 导出全部符号，丢不掉没用的），见生成器注释。\n')
        f.write('# 用到之前请先定义 DM_DIR = 仓库里的 dm/ 目录（与 app 的 CMakeLists 约定一致）；\n')
        f.write('# 本片段自己推出仓库根 DM_REPO = dm/ 的上一层，再用它拼所有路径。\n')
        f.write('get_filename_component(DM_REPO "${DM_DIR}/.." ABSOLUTE)\n\n')

        f.write('# 单测自己的 include 目录\n')
        f.write('set(DM_HOST_TEST_INCS\n')
        for i in incs:
            f.write('    "${DM_REPO}/%s"\n' % i)
        f.write(')\n\n')

        f.write('# 单测需要的宏（上游 Makefile 里那些 -D）\n')
        if defs:
            f.write('set(DM_HOST_TEST_DEFS %s)\n\n' % ' '.join(defs))
        else:
            f.write('set(DM_HOST_TEST_DEFS "")\n\n')

        f.write('# 每个单测 .so 都要带的支撑源：垫片层 + 我们自己的日志/音频\n')
        f.write('set(DM_HOST_TEST_SUPPORT\n')
        for c in compat:
            f.write('    "${DM_REPO}/%s"\n' % c)
        for s in SUPPORT_EXTRA:
            f.write('    "${DM_REPO}/%s"\n' % s)
        f.write(')\n\n')

        f.write('# 单测 .so 清单（与 host_tests_gen.c 里的 lib 名一致）\n')
        f.write('set(DM_HOST_TEST_LIBS\n')
        for n in names:
            f.write('    dmt_%s\n' % n)
        f.write(')\n\n')

        f.write('''# 一段辅助宏：给一个单测 .so 配好 include / 宏 / 链接（**用 macro 不用 function**）。
# 为什么用 macro：macro 在调用者的作用域里展开，读 DM_HOST_TEST_SUPPORT 这类变量不会遇到
# 函数作用域带来的意外；这里宁可直白，也不要那种"看着对、展开后不对"的写法。
macro(dm_host_test_config name)
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")
    # BEFORE + 垫片目录在最前：`#include <TargetConditionals.h>` 之类才会命中替身
    target_include_directories(${name} BEFORE PRIVATE
        "${DM_REPO}/dm/ohos/compat" "${DM_REPO}/dm/ohos" ${DM_HOST_TEST_INCS})
    target_compile_definitions(${name} PRIVATE
        __OHOS__=1 DM_LOG_TO_HILOG=1 _GNU_SOURCE=1 ${DM_HOST_TEST_DEFS})
    target_compile_options(${name} PRIVATE -include shim_prefix.h -w)
    # 只链系统库：每个单测 .so 自成一体，跑测框架按需 dlopen
    target_link_libraries(${name} PUBLIC libhilog_ndk.z.so m)
endmacro()

''')
        # 每个单测一段**独立的 add_library**，不靠变量/函数传参——生成的代码宁可啰嗦，也不要歧义。
        for n in names:
            f.write('# ---- %s ----\n' % n)
            f.write('add_library(dmt_%s SHARED\n' % n)
            for s in rules[n]['srcs']:
                f.write('    "${DM_REPO}/%s"\n' % s)
            for c in compat:
                f.write('    "${DM_REPO}/%s"\n' % c)
            for s in SUPPORT_EXTRA:
                f.write('    "${DM_REPO}/%s"\n' % s)
            f.write(')\n')
            f.write('dm_host_test_config(dmt_%s)\n\n' % n)

    print('生成：')
    print('  dm/ohos/host_tests_gen.c      （%d 个单测）' % len(names))
    print('  dm/ohos/host_tests_gen.cmake  （%d 个 .so 目标）' % len(names))
    if SKIP:
        print('跳过（原因见脚本里的 SKIP）：')
        for k, v in SKIP.items():
            print('  %s —— %s' % (k, v))
    return 0


if __name__ == '__main__':
    sys.exit(main())
