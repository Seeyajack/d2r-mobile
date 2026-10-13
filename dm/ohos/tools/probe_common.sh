#!/usr/bin/env bash
# 探针公共部分：路径推导、配置读取、编译参数、排除判断。
# 被 probe_host.sh（编译探针）与 link_probe.sh（链接探针）共用。
# Shared probe preamble: path derivation, configuration, compile flags, exclusion test. Used by
# probe_host.sh (compile probe) and link_probe.sh (link probe).
#
# **本文件不含任何项目专有内容**——那些都在 probe.conf 里。
# This file contains nothing project-specific; that all lives in probe.conf.
set -u

PROBE_TOOLS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # dm/ohos/tools
DM_OHOS="$(cd "$PROBE_TOOLS/.." && pwd)"                      # dm/ohos
DM_ROOT="$(cd "$DM_OHOS/.." && pwd)"                          # dm
REPO="$(cd "$DM_ROOT/.." && pwd)"                             # 仓库根 | repo root

# 读配置；没有配置文件也能跑（用下面的缺省值）。
CONF="$PROBE_TOOLS/probe.conf"
[ -f "$CONF" ] && . "$CONF"

: "${DM_HOST_DIR:=dm/host}"
: "${DM_INCLUDE_DIRS:=dm/ohos/compat:dm/ohos:dm/host:dm/gfx}"
: "${DM_EXCLUDE:=}"
: "${DM_OHOS_SRCS:=}"
: "${DM_COMPAT_SRC_DIR:=}"
: "${DM_EXTRA_SRCS:=}"
: "${DM_THIRD_PARTY_SRCS:=}"
: "${DM_LINK_LIBS:=}"
: "${DM_DEFINES:=-D__OHOS__=1}"
: "${DM_FORCE_INCLUDE:=}"

# 工具链：缺省用 DevEco 自带的 NDK；用 DM_OHOS_NDK 指到别处也行。
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
CF=(--target=aarch64-linux-ohos "--sysroot=$SYSROOT")
_old_ifs="$IFS"; IFS=':'
for _d in $DM_INCLUDE_DIRS; do CF+=(-I "$REPO/$_d"); done
IFS="$_old_ifs"
for _d in $DM_DEFINES; do CF+=("$_d"); done
[ -n "$DM_FORCE_INCLUDE" ] && CF+=(-include "$DM_FORCE_INCLUDE")

# 这个文件名在排除名单里吗（按 basename 匹配）
probe_excluded() {
    for _x in $DM_EXCLUDE; do [ "$1" = "$_x" ] && return 0; done
    return 1
}

# 把配置里的多行列表变成数组（逐项校验存在性，缺了就报出来而不是静默跳过）
probe_expand_list() {   # $1 = 空格/换行分隔的相对路径串
    for _p in $1; do
        if [ -f "$REPO/$_p" ]; then printf '%s\n' "$REPO/$_p";
        else echo "  （配置里列的文件不存在，已跳过：$_p）" >&2; fi
    done
}
