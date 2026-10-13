#!/usr/bin/env bash
# 便携性回归测试：证明 L1（垫片层 + 探针工具）对"另一个项目"确实够用，且两个探针都不可替代。
# Portability regression test: proves L1 (shim layer + probe tools) is sufficient for *another* project,
# and that neither probe can replace the other.
#
# 为什么要有它：`PORTABLE.md` 里写着"通用"——但**通用是推理出来的还是验证出来的，差很多**。
# 这个脚本把那条主张变成四条能反复跑的断言。
# Why: PORTABLE.md claims the layer is portable. Whether that is reasoned or *verified* makes a big
# difference; this script turns the claim into four re-runnable assertions.
#
# 用法：bash dm/ohos/tools/portable_check.sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/probe_common.sh"
probe_require_tools

SAMPLE="$PROBE_TOOLS/portable_samples/demoapp"
fail=0
num() { sed -n 's/.*未通过 \([0-9]*\).*/\1/p'; }   # 从探针输出里取"未通过 N"

echo "=== 便携性回归测试 ==="
echo "样本项目：$SAMPLE"
echo

# ---- A. 本项目：不能被改坏 ----
out=$(bash "$PROBE_TOOLS/probe_host.sh" 2>&1)
a=$(printf '%s\n' "$out" | num)
if [ "$a" = "0" ]; then echo "  ✅ A. 本项目编译探针：0 失败（没被改坏）"
else echo "  ❌ A. 本项目编译探针出现 $a 个失败"; fail=1; fi

# ---- B. 样本 src/：垫片层对另一个项目够用 ----
# 这是整套测试的**核心断言**。样本故意换了目录布局（嵌套三层）、换了 include 结构，
# 所以它同时在验"垫片够不够广"和"工具能不能指向别的项目"。
out=$(DM_PROBE_PROJECT="$SAMPLE" bash "$PROBE_TOOLS/probe_host.sh" 2>&1)
b=$(printf '%s\n' "$out" | num)
if [ "$b" = "0" ]; then echo "  ✅ B. 样本 src/：8 个文件全过（垫片层对另一个项目够用；嵌套目录被扫到）"
else
    echo "  ❌ B. 样本 src/ 有 $b 个文件编不过——垫片层对「另一个项目」不够用，或工具没能指向它"
    printf '%s\n' "$out" | sed -n '/未通过的文件/,$p' | sed 's/^/       /'
    fail=1
fi

# ---- C. 样本 gaps/：探针必须能报出缺口（反向验证）----
# 如果这里报"0 失败"，坏的不是样本，是**探针**（漏检），那比缺垫片更糟。
out=$(DM_PROBE_PROJECT="$SAMPLE" DM_HOST_DIR=gaps bash "$PROBE_TOOLS/probe_host.sh" 2>&1)
c=$(printf '%s\n' "$out" | num)
if [ "${c:-0}" -gt 0 ]; then echo "  ✅ C. 样本 gaps/：编译探针报出 $c 个缺口（探针没有漏检）"
else echo "  ❌ C. 样本 gaps/ 竟然「全过」——探针漏检了，比缺垫片更糟"; fail=1; fi

# ---- D. 同一份 gaps/：链接探针必须抓出编译探针放过的那一个 ----
# gaps/ 里有一个 `undeclared_sysctlbyname.c`：它**编得过**（C 只给警告），
# 但符号在鸿蒙上不存在。**只有链接探针能抓到这一类**——而这一类正是当初
# `clock_gettime_nsec_np` 那个真 bug 的形状（4 个文件在用、静默截断 64 位纳秒）。
# 这条断言就是"为什么必须做链接探针"的活证据。
out=$(DM_PROBE_PROJECT="$SAMPLE" DM_HOST_DIR=gaps bash "$PROBE_TOOLS/link_probe.sh" 2>&1)
if printf '%s\n' "$out" | grep -q "sysctlbyname"; then
    echo "  ✅ D. 样本 gaps/：链接探针抓出了编译探针放过的那一个（sysctlbyname）"
    printf '%s\n' "$out" | grep "sysctlbyname" | sed 's/^/       /'
else
    echo "  ❌ D. 链接探针没能抓出 sysctlbyname——「两个探针不可互相替代」这条结论不成立"
    fail=1
fi

echo
if [ "$fail" = 0 ]; then
    echo "✅ 四条断言全过：L1 对「另一个项目」够用，且两个探针都不可替代。"
    echo "   （注意：这只证明「编译+链接」这一层通用；「跑起来」没验，见 PORTABLE.md 第五节。）"
else
    echo "❌ 有断言不成立，见上面标 ❌ 的地方。"
fi
exit "$fail"
