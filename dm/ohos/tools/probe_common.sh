#!/usr/bin/env bash
# 探针公共部分：路径推导、配置读取、编译参数、排除判断。
# 被 probe_host.sh（编译探针）与 link_probe.sh（链接探针）共用。
# Shared probe preamble: path derivation, configuration, compile flags, exclusion test. Used by
# probe_host.sh (compile probe) and link_probe.sh (link probe).
#
# **本文件不含任何项目专有内容**——那些都在 probe.conf 里。
# This file contains nothing project-specific; that all lives in probe.conf.
#
# 两个根要分清（这是能探测"别的项目"的关键）：
#   L1 根（本仓库）—— 垫片层和工具住在这里，探测别的项目时**仍然用它**；
#   被测项目根     —— 要检查的那个项目，用 DM_PROBE_PROJECT 指向它，缺省就是本仓库。
# Two roots matter (this is what makes probing another project possible): the L1 root (this repo),
# which holds the shim layer and the tools and is used either way, and the project under test, pointed
# at by DM_PROBE_PROJECT and defaulting to this repo.
set -u

PROBE_TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # dm/ohos/tools
DM_OHOS="$(cd "$PROBE_TOOLS/.." && pwd)"                      # dm/ohos
DM_ROOT="$(cd "$DM_OHOS/.." && pwd)"                          # dm
REPO="$(cd "$DM_ROOT/.." && pwd)"                             # L1 根（本仓库） | the L1 root (this repo)

PROJECT="${DM_PROBE_PROJECT:-$REPO}"                          # 被测项目根 | project under test
[ -d "$PROJECT" ] || { echo "DM_PROBE_PROJECT 不是一个目录：$PROJECT" >&2; exit 2; }
PROJECT="$(cd "$PROJECT" && pwd)"

# ---------------------------------------------------------------- 配置
# 查找顺序：被测项目自己的 probe.conf → 本仓库工具目录里的那份（向后兼容）。
# Lookup order: the project's own probe.conf, then the one next to these tools (backward compatible).
CONF="${DM_PROBE_CONF:-$PROJECT/probe.conf}"
[ -f "$CONF" ] || CONF="$PROBE_TOOLS/probe.conf"

# 环境变量优先于配置文件：先把环境里的值存起来，读完配置再盖回去。
# Environment wins over the config file: save the environment's values, read the config, then re-apply.
_PROBE_VARS="DM_HOST_DIR DM_INCLUDE_DIRS DM_SHIM_DIR DM_EXCLUDE DM_RECURSE DM_OHOS_SRCS DM_COMPAT_SRC_DIR DM_EXTRA_SRCS DM_THIRD_PARTY_SRCS DM_LINK_LIBS DM_DEFINES DM_FORCE_INCLUDE"
declare -A _ENVV=()
for _v in $_PROBE_VARS; do eval "_ENVV[$_v]=\"\${$_v:-}\""; done
[ -f "$CONF" ] && . "$CONF"
for _v in $_PROBE_VARS; do
    eval "_e=\"\${_ENVV[$_v]}\""
    [ -n "$_e" ] && eval "$_v=\"\$_e\""
done

: "${DM_HOST_DIR:=dm/host}"
: "${DM_INCLUDE_DIRS:=}"
: "${DM_SHIM_DIR:=dm/ohos/compat}"
: "${DM_EXCLUDE:=}"
: "${DM_RECURSE:=0}"
: "${DM_OHOS_SRCS:=}"
: "${DM_COMPAT_SRC_DIR:=}"
: "${DM_EXTRA_SRCS:=}"
: "${DM_THIRD_PARTY_SRCS:=}"
: "${DM_LINK_LIBS:=}"
: "${DM_DEFINES:=-D__OHOS__=1}"
: "${DM_FORCE_INCLUDE:=}"

# ---------------------------------------------------------------- 工具链
NDK="${DM_OHOS_NDK:-D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native}"
CLANG="$NDK/llvm/bin/clang.exe"
NM="$NDK/llvm/bin/llvm-nm.exe"
READELF="$NDK/llvm/bin/llvm-readelf.exe"
SYSROOT="$NDK/sysroot"

probe_require_tools() {
    if [ ! -x "$CLANG" ]; then
        echo "找不到鸿蒙 clang：$CLANG" >&2
        echo "用 DM_OHOS_NDK=<.../openharmony/native 目录> 指定，例如：" >&2
        echo "  DM_OHOS_NDK='/d/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native' bash $0" >&2
        exit 2
    fi
}

# 编译参数：用数组，避免工程路径里的空格被拆开。
# 顺序有讲究：**垫片目录必须排在 sysroot 与项目目录之前**，否则 `#include <TargetConditionals.h>`
# 之类会先去找系统目录/项目目录，找不到再报错——那样"零改动替换"就不成立了。
CF=(--target=aarch64-linux-ohos "--sysroot=$SYSROOT")
[ -n "$DM_SHIM_DIR" ] && CF+=(-I "$REPO/$DM_SHIM_DIR")
_old_ifs="$IFS"; IFS=':'
for _d in $DM_INCLUDE_DIRS; do CF+=(-I "$PROJECT/$_d"); done
IFS="$_old_ifs"
CF+=(-I "$PROJECT/$DM_HOST_DIR")
for _d in $DM_DEFINES; do CF+=("$_d"); done
[ -n "$DM_FORCE_INCLUDE" ] && CF+=(-include "$DM_FORCE_INCLUDE")

# 这个文件名在排除名单里吗（按 basename 匹配）
probe_excluded() {
    for _x in $DM_EXCLUDE; do [ "$1" = "$_x" ] && return 0; done
    return 1
}

# 找出被测项目里的源文件。
# 递归还是只扫一层，由 DM_RECURSE 控制（缺省 0 = 只扫一层）。
#
# **为什么要有这个开关**：第一版写死了"只扫一层"（`dir/*.c`），但没人把这个假设写下来。
# 本轮做"换项目"测试时把发现改成递归，本项目数字当场从 67 跳到 98——
# 因为多扫进了 `dm/host/tests/` 下的单测。**假设不写下来，就会在下一次改动里静默失效。**
# 所以：不偷偷改行为，改成显式开关，缺省保持原样。
# Why the switch exists: v1 hardcoded a flat scan and never wrote that assumption down. When this round
# made discovery recursive for the portability test, this project's count jumped 67 -> 98, because the
# unit tests under dm/host/tests/ got pulled in. An unwritten assumption silently breaks on the next
# change -- so it becomes an explicit switch, defaulting to the old behaviour.
# 接一个新项目时通常要打开它（现代项目几乎都分目录）。
probe_sources() {   # $1 = 被测项目下的相对目录；$2 = 扩展名（如 c）
    if [ "$DM_RECURSE" = 1 ]; then
        find "$PROJECT/$1" -type f -name "*.$2" 2>/dev/null | sort
    else
        find "$PROJECT/$1" -maxdepth 1 -type f -name "*.$2" 2>/dev/null | sort
    fi
}

# 把配置里的多行列表变成绝对路径（逐项校验存在性，缺了就报出来而不是静默跳过）
# $1 = 根目录；$2 = 空格/换行分隔的相对路径串
probe_expand_list() {
    for _p in $2; do
        if [ -f "$1/$_p" ]; then printf '%s\n' "$1/$_p"
        else echo "  （配置里列的文件不存在，已跳过：$_p）" >&2; fi
    done
}
