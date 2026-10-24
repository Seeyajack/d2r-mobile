#!/usr/bin/env python3
"""按 dist/README.md 里的清单生成「鸿蒙通用库」交付树。

为什么要有这一步：仓库本身保持**上游镜像 + 我们的增量**（这样 `git merge upstream` 永远无痛），
通用库不靠删目录实现，而是靠清单**生成**出来。详见 dist/README.md。

用法：
    python dist/make_dist.py                    # 生成到 dist/out/
    python dist/make_dist.py --out D:/somewhere # 指定输出目录
    python dist/make_dist.py --check            # 只算不写：报每条的命中数

清单取自 dist/README.md 里 ```manifest 代码块，语法：
    + <glob>    纳入（相对仓库根）
    - <glob>    排除（写在后面的覆盖前面的）
    #           注释
"""
import argparse
import fnmatch
import hashlib
import io
import os
import re
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
README = os.path.join(REPO, 'dist', 'README.md')


def load_rules(path=README):
    """从 README 的 ```manifest 代码块里读出规则（保持"文档即配置"，不用另维护一份）。"""
    s = io.open(path, encoding='utf-8').read()
    m = re.search(r'```manifest\n(.*?)```', s, re.S)
    if not m:
        sys.exit('dist/README.md 里没有 ```manifest 代码块')
    rules = []
    for line in m.group(1).splitlines():
        line = line.split('#', 1)[0].strip()
        if not line or len(line) < 2:
            continue
        sign, pat = line[0], line[1:].strip()
        if sign in '+-' and pat:
            rules.append((sign, pat))
    return rules


def match(rel, pat):
    """glob 语义：`**` 跨目录，`*` 不跨；也接受明确的文件路径。"""
    if pat.endswith('/**'):
        base = pat[:-3]
        return rel == base or rel.startswith(base + '/')
    if pat.endswith('/'):
        return rel.startswith(pat)
    if fnmatch.fnmatchcase(rel, pat):
        return True
    # `dm/host/**` 这类也常被写成 `dm/host/**/*`，两种都认
    return fnmatch.fnmatchcase(rel, pat.replace('/**/', '/**'))


def walk_repo():
    """产出仓库里所有文件（跳过 .git 与 dist/out 自身）。"""
    # 跳过的目录：版本库内部、构建产物（含我们留下的 build.bak-*）、包管理缓存
    skip_dirs = {'.git', 'dist', 'node_modules', 'oh_modules', 'build', '__pycache__'}
    for root, dirs, files in os.walk(REPO):
        rel_root = os.path.relpath(root, REPO).replace('\\', '/')
        if rel_root == '.':
            dirs[:] = [d for d in dirs if d not in skip_dirs]
            rel_root = ''
        dirs[:] = [d for d in dirs if d not in skip_dirs and not d.startswith('build.bak')]
        for f in files:
            yield (rel_root + '/' + f) if rel_root else f


def decide(rel, rules):
    """按顺序套规则：后写的覆盖先写的。返回 (是否纳入, 命中的最后一条规则)。"""
    inc, why = False, None
    for sign, pat in rules:
        if match(rel, pat):
            inc, why = (sign == '+'), '%s %s' % (sign, pat)
    return inc, why


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(REPO, 'dist', 'out'))
    ap.add_argument('--check', action='store_true', help='只统计不写文件')
    args = ap.parse_args()

    rules = load_rules()
    print('清单规则 %d 条（来自 dist/README.md）' % len(rules))

    picked, per_rule = [], {}
    for rel in walk_repo():
        inc, why = decide(rel, rules)
        if inc:
            picked.append(rel)
            per_rule[why] = per_rule.get(why, 0) + 1

    print('入选文件 %d 个' % len(picked))
    print('\n每条规则的命中数：')
    for why, n in sorted(per_rule.items(), key=lambda x: -x[1]):
        print('  %-28s %5d' % (why, n))

    if args.check:
        top = {}
        for rel in picked:
            k = '/'.join(rel.split('/')[:2])
            top[k] = top.get(k, 0) + 1
        print('\n按目录：（前 15）')
        for k, n in sorted(top.items(), key=lambda x: -x[1])[:15]:
            print('  %-28s %5d' % (k, n))
        return

    if os.path.isdir(args.out):
        shutil.rmtree(args.out)
    os.makedirs(args.out, exist_ok=True)
    sha = hashlib.sha256()
    for rel in sorted(picked):
        dst = os.path.join(args.out, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(os.path.join(REPO, rel), dst)
        sha.update(rel.encode('utf-8'))
        with open(os.path.join(REPO, rel), 'rb') as f:
            sha.update(f.read())
    io.open(os.path.join(args.out, 'DIST.sha256'), 'w', encoding='utf-8').write(
        sha.hexdigest() + '  (%d files)\n' % len(picked))
    print('\n已生成：%s' % args.out)
    print('指纹：%s' % sha.hexdigest()[:16])


if __name__ == '__main__':
    main()
