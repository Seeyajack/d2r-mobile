#!/usr/bin/env bash
# 编译探针：拿鸿蒙 NDK 的 clang，把宿主每个 .c 单独过一遍语法与类型检查。
# Compile probe: run every host .c through the HarmonyOS NDK clang individually.
#
# 它回答的问题很具体：**兼容垫片够不够用**。刚接手一个项目时没有人知道它的平台依赖到底有多少，
# 这个脚本给出一张可复现的清单：哪些文件已经能编、哪些还差什么（打印每个失败文件的第一条错误）。
# It answers one concrete question: is the shim layer sufficient? On a cold start nobody knows how many
# platform dependencies a project has, so this prints a reproducible list: what compiles today and, for
# each failure, the first error that stops it.
#
# 用法（Git Bash / WSL / Linux 都行）：
#   bash dm/ohos/tools/probe_host.sh              # 全部
#   bash dm/ohos/tools/probe_host.sh hle_os.c     # 只看某几个文件
# 所有「项目专有清单」（排除名单、include 路径…）在 tools/probe.conf 里，本脚本不含。
# 工具链不在缺省位置时：DM_OHOS_NDK=/path/to/openharmony/native bash ...
#
# 注意：只做 -fsyntax-only（不产出目标文件、不链接）。
# 「能编过」不等于「能跑」，也不等于「能链接成 .so」——链接那一层看 link_probe.sh。
# Note: -fsyntax-only only. "Compiles" is neither "runs" nor "links" -- see link_probe.sh for that layer.
set -u
. "$(cd "$(dirname "$0")" && pwd)/probe_common.sh"
probe_require_tools

HOST="$REPO/$DM_HOST_DIR"
ERR="${TMPDIR:-/tmp}/dm_probe_err.$$"

# 参数：显式指定文件；给相对名就到宿主目录下找
if [ "$#" -gt 0 ]; then
    FILES=()
    for f in "$@"; do
        case "$f" in
            /*|[A-Za-z]:*) FILES+=("$f") ;;
            *) if [ -f "$f" ]; then FILES+=("$f"); else FILES+=("$HOST/$f"); fi ;;
        esac
    done
else
    FILES=("$HOST"/*.c)
fi

ok=0; bad=0; skip=0
FAILED=()
SKIPPED=()
for f in "${FILES[@]}"; do
    base="$(basename "$f")"
    if probe_excluded "$base"; then skip=$((skip + 1)); SKIPPED+=("$base"); continue; fi
    if "$CLANG" "${CF[@]}" -fsyntax-only "$f" 2>"$ERR"; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        FAILED+=("$base|$(grep -m1 'error:' "$ERR" | sed 's/.*error: //')")
    fi
done
rm -f "$ERR"

echo "=============================================="
echo " 编译探针：通过 $ok，未通过 $bad，跳过 $skip（配置里排除的）"
echo " 配置：$CONF"
echo "=============================================="
if [ "$skip" -gt 0 ]; then
    # 跳过的也列出来：排除是对的，但"排除了什么"必须看得见，否则漏掉真问题。
    echo "跳过的文件（配置里认定为「非移植问题」）："
    for s in "${SKIPPED[@]}"; do echo "  $s"; done
fi
if [ "$bad" -gt 0 ]; then
    echo "未通过的文件（首条错误）："
    for line in "${FAILED[@]}"; do printf '  %-26s %s\n' "${line%%|*}" "${line#*|}"; done
fi
[ "$bad" -eq 0 ]
