#!/usr/bin/env bash
# 链接探针：把"能编过的那一批"真的链成一个 .so，看**还缺哪些符号**。
# Link probe: actually link the compilable batch into a .so and see which symbols are still missing.
#
# 为什么要有它：`probe_host.sh` 只做 `-fsyntax-only`，即"每个文件自己讲得通"。
# 但文件之间互相喊名字（甲调乙的函数），这一层只有**真的链一次**才能验。
# 本脚本用 `-Wl,--no-undefined` 把所有没解析到的符号一次性列出来。
# Why: probe_host.sh only does -fsyntax-only ("each file makes sense on its own"). Files call each
# other's functions, and that layer is only verified by actually linking once. --no-undefined makes the
# linker list every unresolved symbol in one go.
#
# 用法：
#   bash dm/ohos/tools/link_probe.sh
# NDK 不在缺省位置时：
#   DM_OHOS_NDK=/path/to/openharmony/native bash dm/ohos/tools/link_probe.sh
#
# 说明：产物放在系统临时目录，**不落在仓库里**（避免文件监控锁住仓库内的 build 目录）。
# Note: artefacts go to the system temp directory, not inside the repo (avoids file-watcher locks on
# an in-repo build directory).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"        # dm/ohos/tools
DM_OHOS="$(cd "$HERE/.." && pwd)"            # dm/ohos
DM_ROOT="$(cd "$DM_OHOS/.." && pwd)"         # dm
HOST="$DM_ROOT/host"
COMPAT="$DM_OHOS/compat"

NDK="${DM_OHOS_NDK:-D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native}"
CLANG="$NDK/llvm/bin/clang.exe"
NM="$NDK/llvm/bin/llvm-nm.exe"
LIBDIR="$NDK/sysroot/usr/lib/aarch64-linux-ohos"
WORK="${TMPDIR:-/tmp}/dm_linkprobe"

if [ ! -x "$CLANG" ]; then
    echo "找不到鸿蒙 clang：$CLANG（用 DM_OHOS_NDK=... 指定）" >&2
    exit 2
fi

# ---- 不参与本次链接的宿主文件 ----
# diag.c / dm_bright.c / dm_vault_ui.c：要用**用户自己的游戏文件**生成的数据表（阶段 2），不是移植问题。
# touch_mac.c：macOS 触摸前端，上游的方案里就写明鸿蒙侧不编它（改用 front_ohos.c）。
# Excluded: the first three need tables generated from the user's own game files (stage 2) and are not a
# porting issue; touch_mac.c is the macOS touch front end, which upstream's own plan says not to build on
# HarmonyOS (front_ohos.c takes its place).
DM_EXCLUDE="diag.c dm_bright.c dm_vault_ui.c touch_mac.c"

rm -rf "$WORK"; mkdir -p "$WORK/obj"

CF=(
    --target=aarch64-linux-ohos
    "--sysroot=$NDK/sysroot"
    -I "$COMPAT"
    -I "$DM_OHOS"
    -I "$HOST"
    -I "$DM_ROOT/gfx"
    -D__OHOS__=1
    -DDM_LOG_TO_HILOG=1
    -D_GNU_SOURCE=1
    -include shim_prefix.h
    -fPIC
    -O0
    -w                       # 这一轮只关心"符号对不对得上"，不看警告 | this round is about symbols, not warnings
)

echo "==== 1/2 编译 ===="
OBJS=()
compile_one() {
    src="$1"; base="$(basename "$src")"; obj="$WORK/obj/${base%.*}.o"
    if "$CLANG" "${CF[@]}" -c "$src" -o "$obj" 2>"$WORK/err.txt"; then
        echo "$obj"
    else
        echo "  ❌ 编不过：$base" >&2
        head -3 "$WORK/err.txt" >&2
    fi
}
export -f compile_one
export CLANG WORK
export CF_STR="${CF[*]}"

# 宿主 .c（排除 3 个等游戏文件的）+ entry.S + dm/ohos 侧 + 垫片层
SRCS=()
for f in "$HOST"/*.c; do
    b="$(basename "$f")"
    skip=0
    for x in $DM_EXCLUDE; do [ "$b" = "$x" ] && skip=1; done
    [ "$skip" = 0 ] && SRCS+=("$f")
done
SRCS+=("$HOST/entry.S")
SRCS+=("$DM_OHOS/front_ohos.c" "$DM_OHOS/dm_log.c" "$DM_OHOS/audio_ohos.c" "$DM_OHOS/map_image_ohos.c")
SRCS+=("$COMPAT"/src/*.c)
# 触摸控制模块：上游把它放在 dm/ios/Sources/ 下，但它只包含自己的头 + host/dm_input.h + 标准库，
# 是**平台无关**的 .c（实测在鸿蒙侧原样编过）⇒ 直接编进来，不用移植。
# The touch-controls module sits under dm/ios/Sources/ upstream but includes only its own header,
# host/dm_input.h and standard headers, so it is platform-neutral (verified to compile as is on
# HarmonyOS) and is simply built rather than ported.
SRCS+=("$DM_ROOT/ios/Sources/dm_touch_controls.c")
# 第三方 libdeflate：源文件清单照抄上游 dm/host/Makefile 的 LIBDEFLATE 变量（只编解压相关部分）。
# Third-party libdeflate: the source list is copied from the LIBDEFLATE variable in the upstream
# dm/host/Makefile (decompression-related sources only).
TP="$DM_ROOT/third_party/libdeflate/lib"
SRCS+=("$TP/deflate_decompress.c" "$TP/zlib_decompress.c" "$TP/adler32.c" "$TP/utils.c" "$TP/arm/cpu_features.c")

n=0
for src in "${SRCS[@]}"; do
    base="$(basename "$src")"; obj="$WORK/obj/${base%.*}.o"
    if "$CLANG" "${CF[@]}" -c "$src" -o "$obj" 2>"$WORK/err_$n.txt"; then
        OBJS+=("$obj")
    else
        echo "  ❌ 编不过：$base" >&2
        grep -m2 "error:" "$WORK/err_$n.txt" | sed 's/^/       /' >&2
    fi
    n=$((n + 1))
done
echo "  编译完成：${#OBJS[@]} 个目标文件"

echo "==== 2/2 链接（-Wl,--no-undefined，把缺的符号全列出来）===="
# 系统库：sysroot 里都有（含 libace_*/libhilog_ndk/libnative_window/libohaudio）
LIBS=(-lz -lace_napi.z -lace_ndk.z -lhilog_ndk.z -lnative_window -lohaudio)
# --error-limit=0 很重要：ld.lld 缺省报满 20 条就停了（"too many errors emitted"），
# 不关掉的话会把"比 20 多得多"当成"只有 20 个"，得出完全错的结论。
# --error-limit=0 matters: ld.lld stops after 20 errors by default ("too many errors emitted"), and
# without disabling it a much larger count looks like exactly 20 -- a completely wrong conclusion.
"$CLANG" --target=aarch64-linux-ohos "--sysroot=$NDK/sysroot" \
    -shared -Wl,--no-undefined -Wl,-z,defs -Wl,--error-limit=0 \
    "${OBJS[@]}" "${LIBS[@]}" -o "$WORK/libdmohos_probe.so" 2>"$WORK/link_err.txt"
rc=$?

if [ $rc -eq 0 ]; then
    echo "✅ 链接成功，没有未解析符号：$WORK/libdmohos_probe.so"
    exit 0
fi

echo "❌ 有未解析符号。每个符号后面列出「引用了它的目标文件」——数据来自 llvm-nm，不是我猜的。"
echo
grep -o "undefined symbol: [^ ]*" "$WORK/link_err.txt" | sed 's/undefined symbol: //' | sort -u > "$WORK/undef.txt"
echo "  合计 $(wc -l < "$WORK/undef.txt" | tr -d ' ') 个"
echo

# 建一张「符号 → 引用它的目标文件」的表，这样分类是数据驱动的，换一批文件也不会误判。
# Build a "symbol -> referencing object" table so the classification is data-driven and does not
# misjudge when the file set changes.
: > "$WORK/refs.txt"
for o in "${OBJS[@]}"; do
    "$NM" -u "$o" 2>/dev/null | awk -v ob="$(basename "$o" .o)" '{print $2" "ob}' >> "$WORK/refs.txt"
done

while read -r sym; do
    [ -z "$sym" ] && continue
    who=$(awk -v s="$sym" '$1==s {printf "%s ", $2}' "$WORK/refs.txt" | sed 's/ *$//')
    printf '  %-36s ← %s\n' "$sym" "${who:-（找不到，可能来自汇编）}"
done < "$WORK/undef.txt"

echo
echo "完整链接日志：$WORK/link_err.txt"
echo
echo "提示：判读方法看 dm/ohos/compat/README.md——"
echo "  · 引用者里有 .m 或 gfx 侧的：图形层还没做，属预期缺口"
echo "  · 引用者是被排除的 3 个文件（diag/dm_bright/dm_vault_ui）：等游戏文件，不是移植问题"
echo "  · 其余的就是真缺口，要补垫片或补实现"
