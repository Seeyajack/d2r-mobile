#!/usr/bin/env bash
# 链接探针：把"能编过的那一批"真的链成一个 .so，看**还缺哪些符号**。
# Link probe: actually link the compilable batch into a .so and see which symbols are still missing.
#
# 为什么要有它：编译探针只做 -fsyntax-only，即"每个文件自己讲得通"。
# 但文件之间互相喊名字（甲调乙的函数），这一层只有**真的链一次**才能验。
# 用 -Wl,--no-undefined 把所有没解析到的符号一次性列出来，并附上"谁在引用它"（数据来自 llvm-nm）。
# Why: the compile probe only does -fsyntax-only ("each file makes sense on its own"). Files call each
# other's functions, and that layer is verified only by actually linking once. --no-undefined lists every
# unresolved symbol, together with who references it (data from llvm-nm, not guesswork).
#
# 用法：
#   bash dm/ohos/tools/link_probe.sh
# 所有"项目专有清单"在 tools/probe.conf 里，本脚本不含。
#
# 产物放在系统临时目录，**不落在仓库里**（避免文件监控锁住仓库内的 build 目录）。
# Artefacts go to the system temp directory, never inside the repo (avoids file-watcher locks).
set -u
. "$(cd "$(dirname "$0")" && pwd)/probe_common.sh"
probe_require_tools

HOST="$REPO/$DM_HOST_DIR"
WORK="${TMPDIR:-/tmp}/dm_linkprobe"

rm -rf "$WORK"; mkdir -p "$WORK/obj"

# 这一轮只关心"符号对不对得上"，不看警告
CF+=(-fPIC -O0 -w)

# ---- 收集源文件 ----
SRCS=()
for f in "$HOST"/*.c; do
    probe_excluded "$(basename "$f")" || SRCS+=("$f")
done
[ -f "$HOST/entry.S" ] && SRCS+=("$HOST/entry.S")

while read -r p; do [ -n "$p" ] && SRCS+=("$p"); done < <(probe_expand_list "$DM_OHOS_SRCS")
[ -n "$DM_COMPAT_SRC_DIR" ] && for f in "$REPO/$DM_COMPAT_SRC_DIR"/*.c; do [ -f "$f" ] && SRCS+=("$f"); done
while read -r p; do [ -n "$p" ] && SRCS+=("$p"); done < <(probe_expand_list "$DM_EXTRA_SRCS")
while read -r p; do [ -n "$p" ] && SRCS+=("$p"); done < <(probe_expand_list "$DM_THIRD_PARTY_SRCS")

echo "==== 1/2 编译 ===="
OBJS=()
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

echo "==== 2/2 链接 ===="
# --error-limit=0 很重要：ld.lld 缺省报满 20 条就停了（"too many errors emitted"），
# 不关掉的话会把"比 20 多得多"当成"只有 20 个"，得出完全错的结论。
# --error-limit=0 matters: ld.lld stops after 20 errors by default ("too many errors emitted"), and
# without disabling it a much larger count looks like exactly 20 -- a completely wrong conclusion.
LIBS=()
for l in $DM_LINK_LIBS; do LIBS+=("$l"); done

"$CLANG" --target=aarch64-linux-ohos "--sysroot=$SYSROOT" \
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
echo "  合计 $(wc -l < "$WORK/undef.txt" | tr -d ' ') 个（配置：$CONF）"
echo

# 建一张「符号 → 引用它的目标文件」的表，这样分类是数据驱动的，换一批文件也不会误判。
: > "$WORK/refs.txt"
for o in "${OBJS[@]}"; do
    "$NM" -u "$o" 2>/dev/null | awk -v ob="$(basename "$o" .o)" '{print $2" "ob}' >> "$WORK/refs.txt"
done

while read -r sym; do
    [ -z "$sym" ] && continue
    who=$(awk -v s="$sym" '$1==s {printf "%s ", $2}' "$WORK/refs.txt" | sed 's/ *$//')
    printf '  %-38s ← %s\n' "$sym" "${who:-（找不到，可能来自汇编）}"
done < "$WORK/undef.txt"

echo
echo "完整链接日志：$WORK/link_err.txt"
echo
echo "判读方法（见 PORTABLE.md 与 dm/ohos/链接现状与缺口.md）："
echo "  · 引用者在配置文件排除名单里的：等「程序专属数据」，不是移植问题"
echo "  · 引用者属于尚未开工的那一层（如图形层）：预期缺口"
echo "  · 其余的就是真缺口，要补垫片或补实现"
