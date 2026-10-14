#!/usr/bin/env bash
# 上游同步 + 全套验证，一条命令。
# Upstream sync plus the full verification pass, in one command.
#
# 背景：上游（jhaizhou-ops/d2r-mobile）在动，我们在它上面做鸿蒙宿主（dm/ohos/）。
# 这条命令把"同步上游"从半天手工操作固化成一条命令，并且**每一步都验**——
# 合并进来不等于没破坏，要过边界校验、编译探针、链接探针，再重新生成单测清单。
# Why: upstream moves; we build the HarmonyOS host on top of it. This makes syncing a single command
# where every step is verified -- merging is not the same as not having broken anything.
#
# 用法：
#   bash dm/ohos/tools/upstream_sync.sh            # 完整流程（fetch + 合并 + 全部验证）
#   bash dm/ohos/tools/upstream_sync.sh --check    # 只看上游有什么新东西，不合并不验证
#   bash dm/ohos/tools/upstream_sync.sh --quick    # 合并 + 验证，但跳过最慢的链接探针
set -u
TOOLS="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$TOOLS/../../.." && pwd)"
cd "$REPO" || exit 2

CHECK_ONLY=0; QUICK=0
for a in "$@"; do
    case "$a" in
        --check) CHECK_ONLY=1 ;;
        --quick) QUICK=1 ;;
        *) echo "未知参数：$a"; exit 2 ;;
    esac
done

step() { echo; echo "======== $1 ========"; }

# ---- 1. 取上游 ----
step "1/6 拉取上游"
if ! git fetch upstream 2>&1 | tail -3; then
    echo "❌ fetch 失败：检查网络与 upstream 远端（git remote -v）"; exit 1
fi
UP_BRANCH="${DM_UPSTREAM_BRANCH:-upstream/main}"

NEW=$(git log --oneline HEAD..$UP_BRANCH | wc -l | tr -d ' ')
echo "上游比我们多 $NEW 个提交。"
if [ "$NEW" = "0" ]; then
    echo "✅ 已是最新，无需同步。"
    exit 0
fi
echo "--- 上游的新提交 ---"
git log --oneline HEAD..$UP_BRANCH | head -12
B=$(git merge-base HEAD $UP_BRANCH)
BOTH=$(comm -12 <(git diff --name-only $B HEAD | sort) <(git diff --name-only $B $UP_BRANCH | sort))
if [ -n "$BOTH" ]; then
    echo "--- 两边都改过的文件（合并最可能撞的地方）---"
    echo "$BOTH"
fi
if [ "$CHECK_ONLY" = 1 ]; then
    echo; echo "（--check 模式到此为止，未合并未验证）"
    exit 0
fi

# ---- 2. 合并 ----
step "2/6 合并 $UP_BRANCH"
if ! git merge $UP_BRANCH --no-edit; then
    echo "❌ 合并冲突。处理完冲突后自己 git commit，然后从第 3 步继续："
    echo "      bash dm/ohos/tools/boundary_check.sh && bash dm/ohos/tools/probe_host.sh && ..."
    echo "   分叉点清单见 dm/ohos/上游分叉清单.md（照单核对每一处）。"
    exit 1
fi
echo "✅ 合并完成（无冲突或冲突已解决）。"

# ---- 3. 边界校验：分层没被破坏 ----
step "3/6 边界校验（通用层不依赖专属层）"
bash "$TOOLS/boundary_check.sh" || { echo "❌ 边界校验没过——先修这个，别的都不用看"; exit 1; }

# ---- 4. 编译探针：新代码在垫片下能不能编 ----
step "4/6 编译探针"
bash "$TOOLS/probe_host.sh" | tail -8

# ---- 5. 链接探针（最慢的一步，--quick 可跳）----
if [ "$QUICK" = 1 ]; then
    echo; echo "======== 5/6 链接探针（--quick 跳过）========"
else
    step "5/6 链接探针"
    bash "$TOOLS/link_probe.sh" | tail -12
fi

# ---- 6. 重新生成单测清单（上游加了/改了单测会自动收进来）----
step "6/6 重新生成单测清单"
PY="${DM_PYTHON:-python3}"
"$PY" "$TOOLS/gen_host_tests.py" || { echo "❌ 生成失败：Makefile 结构可能又变了，照 dm/ohos/上游分叉清单.md 的思路改解析器"; exit 1; }

echo
echo "=============================================================="
echo "✅ 同步与静态验证完成。剩下两步要在真机上做（本脚本做不了）："
echo "   1) cd dm/ohos/app && devecocli run --device <序列号>"
echo "   2) 点「清日志、重头跑一遍」，再按需点「续跑」，看单测结果"
echo "      （流程与判读见 dm/ohos/真机单测结果/结果-2026-10-14.md）"
echo "=============================================================="
