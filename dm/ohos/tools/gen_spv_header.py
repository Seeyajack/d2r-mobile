#!/usr/bin/env python3
"""把官方 glslang 编出的 .spv 变成 C 头文件里的 uint32_t 数组。

用 C 头文件把 SPIR-V 嵌进代码，是这个项目一直的做法（见 tri_shaders.h）：NDK 里没有 shaderc，
所以着色器**只能在电脑侧用官方 glslang 编译**，产物当**地面真值**——手写 SPIR-V 在真机上
会让渲染服务直接崩（dm/ohos/图形层实施方案-Vulkan.md 铁律 2）。
几百个 32 位字手工抄进头文件既烦又容易错，所以这一步交给脚本；生成的头文件开头写明"别手改"。

用法：
    python gen_spv_header.py <输出.h> <变量名>=<文件.spv> [<变量名>=<文件.spv> ...]

例：
    python gen_spv_header.py dm/ohos/tex_shaders.h \
        dm_spv_tex_vert=dm/ohos/shaders/tex.vert.spv \
        dm_spv_tex_frag=dm/ohos/shaders/tex.frag.spv

生成的内容形如（与 tri_shaders.h 同一风格）：
    static const uint32_t dm_spv_tex_vert[] = { 0x07230203, ... };
    static const uint32_t dm_spv_tex_vert_n = (uint32_t)(sizeof dm_spv_tex_vert / sizeof dm_spv_tex_vert[0]);
"""
import os
import struct
import sys


def load_spv(path):
    """读 .spv 并解析成 32 位字。SPIR-V 是小端字节序的 32 位字流。"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) % 4 != 0:
        raise SystemExit(f"{path}: 大小不是 4 的倍数（{len(data)}），不像 SPIR-V")
    n = len(data) // 4
    words = struct.unpack("<%dI" % n, data)
    # SPIR-V 头部五个字：magic / version / generator / bound / schema（**版本在 word[1]，不是 word[3]**）
    if words[0] != 0x07230203:
        raise SystemExit(f"{path}: 魔数不对（0x{words[0]:08x}），不是 SPIR-V")
    ver = words[1]
    print(f"  {os.path.basename(path)}: {n} 字，SPIR-V {ver >> 16}.{(ver >> 8) & 0xff}，"
          f"生成器 0x{words[2]:08x}，边界 {words[3]}")
    return words


def c_array(name, words):
    body = "".join("0x%08x,\n" % w for w in words)
    return (f"static const uint32_t {name}[] = {{\n{body}}};\n"
            f"static const uint32_t {name}_n = (uint32_t)(sizeof {name} / sizeof {name}[0]);\n")


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    out_path, pairs = argv[1], argv[2:]

    chunks = []
    for spec in pairs:
        if "=" not in spec:
            raise SystemExit(f"参数格式应为 变量名=文件.spv，收到：{spec}")
        name, spv = spec.split("=", 1)
        if not name.isidentifier():
            raise SystemExit(f"变量名不合法：{name}")
        words = load_spv(spv)
        chunks.append((name, spv, words))

    lines = [
        "// 【生成文件，别手改】glslang 编出的标准 SPIR-V —— 地面真值。",
        "// GENERATED -- do not edit by hand. SPIR-V produced by the official glslang; this is ground truth.",
        "// 再生成：",
        "//   python dm/ohos/tools/gen_spv_header.py dm/ohos/tex_shaders.h \\",
    ] + [f"//       {n}={s}" for n, s, _ in chunks] + [
        "// 源在 dm/ohos/shaders/；改源之后必须重新编译并重新生成，**不要手改本文件**。",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    for name, src, words in chunks:
        lines.append(f"// ---- {src} ----")
        lines.append(c_array(name, words).rstrip("\n"))
        lines.append("")

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines).rstrip("\n") + "\n")
    print(f"已写出 {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
