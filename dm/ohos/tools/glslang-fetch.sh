#!/usr/bin/env bash
# 取回 glslang（GLSL → SPIR-V 的 Khronos 官方编译器）。
# 用途：图形层阶段 2 起，SPIR-V 必须在电脑侧编译、以官方产物为地面真值——
# 驱动编译畸形 SPIR-V 会崩掉整个渲染服务（不是可恢复的 VkResult），所以不能在设备上试错。
# 用法：bash dm/ohos/tools/glslang-fetch.sh [目标目录]（缺省 <项目根>/tools/glslang-16.6.0）
set -u
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)   # 项目根（仓库外面，工具不进 git）
DEST="${1:-$ROOT/tools/glslang-16.6.0}"
if [ -x "$DEST/bin/glslang.exe" ]; then echo "✅ 已存在：$DEST/bin/glslang.exe"; exit 0; fi
URL="https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip"
mkdir -p "$DEST" && cd "$DEST" || exit 2
echo "下载 $URL"
curl -sL -o glslang.zip "$URL" || exit 1
python -c "import zipfile; zipfile.ZipFile('glslang.zip').extractall('.')" || exit 1
rm -f glslang.zip
./bin/glslang.exe --version | head -2 && echo "✅ 就绪：$DEST/bin/glslang.exe"
