#!/usr/bin/env bash
# 宿主单测的**链接预检**：把 27 个单测（各自 main 已改名）编成一个 .so，看能不能拼到一起。
# Link pre-check for the host unit tests: build all 27 (each with its main renamed) into one .so.
#
# 为什么要单独一个：单测彼此之间可能撞符号（同名全局、重复依赖的 .c），
# 而"撞车"只有在**链接**时才暴露。先在命令行把它验掉，比每次走完整 App 构建快得多。
# Why separate: the tests can collide (same-named globals, shared .c dependencies), and collisions only
# show up at link time. Catching it on the command line beats a full app build every iteration.
#
# 清单来自 dm/ohos/host_tests_gen.cmake（由 gen_host_tests.py 从 Makefile 生成）——本脚本不含列表。
# The roster comes from host_tests_gen.cmake (generated from the Makefile); this script holds no list.
#
# 用法：bash dm/ohos/tools/link_probe_tests.sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/probe_common.sh"
probe_require_tools

GEN="$DM_OHOS/host_tests_gen.cmake"
[ -f "$GEN" ] || { echo "缺 $GEN：先跑 python3 dm/ohos/tools/gen_host_tests.py" >&2; exit 2; }

WORK="${TMPDIR:-/tmp}/dm_testlink"
rm -rf "$WORK"; mkdir -p "$WORK/obj"

CF+=(-fPIC -O0 -w -DDM_TEST_HOOKS)
[ -d "$REPO/dm/host" ] && CF+=(-I "$REPO/dm/host")
[ -d "$REPO/dm/ios/Sources" ] && CF+=(-I "$REPO/dm/ios/Sources")

# 从生成的 cmake 里读"哪个源要改名成哪个符号"
declare -A RENAME=()
while IFS= read -r line; do
    src=$(printf '%s' "$line" | sed -n 's/.*"\${DM_DIR}\/\([^"]*\)" PROPERTIES.*/\1/p')
    sym=$(printf '%s' "$line" | sed -n 's/.*COMPILE_DEFINITIONS "main=\([^"]*\)".*/\1/p')
    [ -n "$src" ] && [ -n "$sym" ] && RENAME["$src"]="$sym"
done < "$GEN"

# 源码清单也来自同一个文件
SRCS=$(sed -n '/^set(DM_HOST_TEST_SRCS/,/^)/p' "$GEN" | sed -n 's/.*"\${DM_DIR}\/\([^"]*\)".*/\1/p')
EXTRA=$(sed -n '/^set(DM_HOST_TEST_INCS/,/^)/p' "$GEN" | sed -n 's/.*"\${DM_DIR}\/\([^"]*\)".*/\1/p')
for e in $EXTRA; do CF+=(-I "$REPO/$e"); done

echo "==== 编译（$(printf '%s\n' "$SRCS" | wc -l | tr -d ' ') 个单测源 + 框架）===="
OBJS=()
n=0
for s in $SRCS; do
    obj="$WORK/obj/$(printf '%s' "$s" | tr '/' '_' | sed 's/\.c$/.o/')"
    fl=("${CF[@]}")
    [ -n "${RENAME[$s]:-}" ] && fl+=("-Dmain=${RENAME[$s]}")
    if "$CLANG" "${fl[@]}" -c "$REPO/$s" -o "$obj" 2>"$WORK/e$n.txt"; then
        OBJS+=("$obj")
    else
        echo "  ❌ 编不过：$s" >&2
        grep -m2 "error:" "$WORK/e$n.txt" | sed 's/^/       /' >&2
    fi
    n=$((n + 1))
done
for f in "$DM_OHOS/host_tests.c" "$DM_OHOS/host_tests_gen.c"; do
    obj="$WORK/obj/$(basename "$f" .c).o"
    if "$CLANG" "${CF[@]}" -c "$f" -o "$obj" 2>"$WORK/f$n.txt"; then OBJS+=("$obj")
    else echo "  ❌ 编不过：$(basename "$f")" >&2; grep -m2 "error:" "$WORK/f$n.txt" | sed 's/^/       /' >&2; fi
    n=$((n + 1))
done
for f in "$DM_OHOS/compat/src"/*.c; do
    obj="$WORK/obj/$(basename "$f" .c).o"
    "$CLANG" "${CF[@]}" -c "$f" -o "$obj" 2>/dev/null && OBJS+=("$obj")
done
echo "  编译完成：${#OBJS[@]} 个目标文件"
echo "  （有改名入口的单测：${#RENAME[@]} 个）"

if [ "${#OBJS[@]}" -eq 0 ]; then
    echo "❌ 一个目标文件都没编出来——先看上面的编译错误，别把「空链接成功」当成通过"
    exit 1
fi

echo "==== 链接 ===="
"$CLANG" --target=aarch64-linux-ohos "--sysroot=$SYSROOT" \
    -shared -Wl,--no-undefined -Wl,-z,defs -Wl,--error-limit=0 \
    "${OBJS[@]}" -lm -lz -o "$WORK/libdmhosttests.so" 2>"$WORK/link.txt"
if [ $? -eq 0 ]; then
    echo "✅ 链接成功：$WORK/libdmhosttests.so"
    "$NM" -D "$WORK/libdmhosttests.so" 2>/dev/null | grep -c " T dm_test_.*_main" | xargs echo "  导出的单测入口数："
else
    echo "❌ 未解析/冲突的符号："
    grep -oE "(undefined symbol: [^ ]*|duplicate symbol: [^ ]*)" "$WORK/link.txt" | sed 's/.*: //' | sort -u | head -25 | sed 's/^/  /'
    echo "  完整日志：$WORK/link.txt"
fi
