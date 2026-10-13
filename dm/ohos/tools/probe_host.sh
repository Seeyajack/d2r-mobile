#!/usr/bin/env bash
# 宿主"能不能编过"探针：拿鸿蒙 NDK 的 clang，把 dm/host 下每个 .c 单独过一遍语法与类型检查。
# Probe: run every dm/host/.c through the HarmonyOS NDK clang individually.
#
# 它回答的问题很具体：**兼容垫片够不够用**。刚移植时没有人知道宿主的苹果依赖到底有多少，
# 这个脚本给出一张可复现的清单：哪些文件已经能编、哪些还差什么（打印每个失败文件的头一条错误）。
# It answers one concrete question: is the shim layer sufficient? Given a cold start, nobody knows how
# many Apple dependencies the host really has, so this prints a reproducible list: what compiles today
# and, for each failure, the first error that stops it.
#
# 用法（Git Bash / WSL / Linux 都行）：
#   bash dm/ohos/tools/probe_host.sh              # 全部
#   bash dm/ohos/tools/probe_host.sh hle_os.c     # 只看某几个文件
# NDK 不在缺省位置时用环境变量指定：
#   DM_OHOS_NDK=/path/to/openharmony/native bash dm/ohos/tools/probe_host.sh
#
# 注意：这只做 -fsyntax-only（不产出目标文件、不链接）。
# "能编过"不等于"能跑"，也不等于"能链接成 .so"——见 ../CMakeLists.txt 的说明。
# Note: this is -fsyntax-only (no object files, no linking). "Compiles" is neither "runs" nor "links".
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"        # dm/ohos/tools
DM_OHOS="$(cd "$HERE/.." && pwd)"            # dm/ohos
DM_ROOT="$(cd "$DM_OHOS/.." && pwd)"         # dm
HOST="$DM_ROOT/host"
COMPAT="$DM_OHOS/compat"

NDK="${DM_OHOS_NDK:-D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native}"
CLANG="$NDK/llvm/bin/clang.exe"
SYSROOT="$NDK/sysroot"

if [ ! -x "$CLANG" ]; then
    echo "找不到鸿蒙 clang：$CLANG" >&2
    echo "请用 DM_OHOS_NDK=<openharmony/native 目录> 指定，例如：" >&2
    echo "  DM_OHOS_NDK='/d/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native' bash $0" >&2
    exit 2
fi

# 参数顺序有讲究：-I 垫片目录必须在 sysroot 之前（用数组避免路径里的空格被拆开）。
# Argument order matters: the shim -I must come before the sysroot. An array keeps paths with spaces intact.
CF=(
    --target=aarch64-linux-ohos
    "--sysroot=$SYSROOT"
    -I "$COMPAT"
    -I "$DM_OHOS"
    -I "$HOST"
    -I "$DM_ROOT/gfx"
    -D__OHOS__=1
    -DDM_LOG_TO_HILOG=1
    -include shim_prefix.h
)

if [ "$#" -gt 0 ]; then
    FILES=("$@")
else
    FILES=("$HOST"/*.c)
fi

ok=0; bad=0
FAILED=()
for f in "${FILES[@]}"; do
    case "$f" in
        /*|[A-Za-z]:*) path="$f" ;;
        *)            path="$HOST/$f" ;;
    esac
    [ -f "$path" ] || { echo "跳过（不存在）：$path" >&2; continue; }
    if "$CLANG" "${CF[@]}" -fsyntax-only "$path" 2>/tmp/dmprobe_err.$$; then
        ok=$((ok + 1))
    else
        bad=$((bad + 1))
        first="$(grep -m1 'error:' /tmp/dmprobe_err.$$ | sed 's/.*error: //')"
        FAILED+=("$(basename "$path")|$first")
    fi
    rm -f /tmp/dmprobe_err.$$
done

echo "=============================================="
echo " 宿主编译探针：共 $((ok + bad)) 个文件，通过 $ok，未通过 $bad"
echo "=============================================="
if [ "$bad" -gt 0 ]; then
    echo "未通过的文件（首条错误）："
    for line in "${FAILED[@]}"; do
        printf '  %-24s %s\n' "${line%%|*}" "${line#*|}"
    done
fi
[ "$bad" -eq 0 ]
