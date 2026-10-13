#!/usr/bin/env bash
# 边界校验：确认 PORTABLE.md 里那条"通用 / 专属"的线没有被破坏。
# Boundary check: verify the portable/game-specific line described in PORTABLE.md has not been broken.
#
# 为什么要有它：边界写在文档里是**会烂掉的**——今天对，改几行就悄悄不对了，
# 而"悄悄"正是最贵的：等换第二个项目时才发现通用层早就依赖了专属内容，那时改起来已经晚了。
# 这个脚本把那条线变成**每次都能跑一遍的检查**。
# Why: a boundary written only in a document rots. It is right today and quietly wrong after a few edits,
# and "quietly" is the expensive part -- you find out when porting a second project, far too late. This
# turns the line into something you can re-run any time.
#
# 用法：bash dm/ohos/tools/boundary_check.sh
set -u
. "$(cd "$(dirname "$0")" && pwd)/probe_common.sh"

fail=0
say() { printf '%s\n' "$*"; }

say "=== 边界校验 ==="
say "配置：$CONF"
say

# ---------------------------------------------------------------- 检查 1
# L1（垫片层 dm/ohos/compat/）必须**独立**：它不能包含上游宿主的任何头。
# 因为 L1 的卖点是"搬到任何项目都能用"——一旦它 include 了 dm.h / hle.h，
# 它就只能服务于这套 Horizon 宿主，跨领域复用当场作废。
#
# 判据是**数据驱动**的：不是拿名字猜，而是看"这个名字的头是不是真的存在于上游 dm/host/ 里"。
# （第一版用正则猜，把自家的 dm_compat_types.h 误判成了上游头——所以改成这样。）
say "---- 检查 1：L1（垫片层）不得包含上游宿主的头 ----"
viol=0
while IFS= read -r f; do
    while IFS= read -r inc; do
        [ -z "$inc" ] && continue
        if [ -f "$REPO/dm/host/$(basename "$inc")" ]; then
            viol=1
            say "  ❌ ${f#$REPO/}  →  #include \"$inc\"（该头在上游 dm/host/ 里）"
        fi
    done < <(grep -oE '#include[[:space:]]*"[^"]+"' "$f" 2>/dev/null | sed 's/.*"\(.*\)"/\1/')
done < <(find "$DM_OHOS/compat" -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null)
[ "$viol" = 0 ] && say "  ✅ 通过（垫片层只依赖自己的头与系统头）" || fail=1
say

# ---------------------------------------------------------------- 检查 2
# L1+L2（dm/ohos 下的源文件，不含 app/ 脚手架）不得引用 L3 的内容。
# L3 的标志物 = probe.conf 里那份排除名单（那些文件之所以被排除，正是因为它们属于专属层）。
say "---- 检查 2：L1+L2 不得引用 L3（排除名单里的文件）----"
viol=0
for name in $DM_EXCLUDE; do
    # 只按"被 include 或被当作头引用"来判，避免把注释里的说明也算成违规
    hits=$(grep -rn "#include[[:space:]]*\"$name\"" "$DM_OHOS" --include='*.c' --include='*.h' \
           --exclude-dir=app 2>/dev/null || true)
    if [ -n "$hits" ]; then
        viol=1
        say "  ❌ 引用了 L3 文件 $name："
        printf '%s\n' "$hits" | sed "s|$REPO/||; s/^/       /"
    fi
done
[ "$viol" = 0 ] && say "  ✅ 通过" || fail=1
say

# ---------------------------------------------------------------- 检查 3
# 每个 L1/L2 源文件都该能在"只给垫片 + NDK"的条件下编过（这正是 probe_host / link_probe 在验的）。
# 这里不重复跑编译，只提示一句：改动后记得跑那两个探针。
say "---- 检查 3：改动后请跑两个探针（本脚本不重复编译）----"
say "  bash dm/ohos/tools/probe_host.sh    # 每个文件自己讲得通吗"
say "  bash dm/ohos/tools/link_probe.sh    # 文件之间喊的名字对得上吗"
say

if [ "$fail" = 0 ]; then
    say "✅ 边界完好：通用层没有依赖专属层。"
else
    say "❌ 边界被破坏：上面标 ❌ 的地方要么改回去，要么把它们登记为「属于哪一层」。"
    say "   判据见 PORTABLE.md 第二节的三个问题。"
fi
exit "$fail"
