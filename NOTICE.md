# 来源声明 / Attribution Notice

## 本项目是什么

本项目是基于 [jhaizhou-ops/d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)
的 **HarmonyOS NEXT（纯血鸿蒙）移植分支**。

- **上游项目**：https://github.com/jhaizhou-ops/d2r-mobile
- **上游许可证**：MIT License
- **上游版权**：Copyright (c) jhaizhou-ops
- **本分支仓库**：https://github.com/Seeyajack/d2r-mobile（开发分支：`harmony`）

## 我们改了什么

本分支在上游基础上新增或修改的部分：

- 针对鸿蒙内核 ABI 重新推导的程序改写规则集
- 鸿蒙宿主层（以鸿蒙 NDK / musl libc 替代 Apple libc 与系统框架）
- Vulkan 图形后端（替代上游的 Metal 后端）及对应的着色器翻译链后端
- ArkTS + XComponent 应用外壳与触屏交互

上游未改动的部分，版权与许可证仍归上游作者所有。

## 参考与致谢（同源分支 nxnative）

[seedds/nxnative](https://github.com/seedds/nxnative)（MIT）是**从上游 d2r-mobile 派生**的通用化分支：
它把同一套做法从"移植某一款游戏"推广到"任意 Switch 游戏"（NSP → IPA，在 iPhone/iPad/Mac 上原生运行），
同样是 NSO 改写器 + Horizon HLE 宿主 + NVN→图形后端，**不是模拟器**。

我们参考了它的公开资料（README、`dm/host/` 目录构成、`hle_os.c` 顶部注释、`docs/agents/`），
得到一个关键技术结论：**Switch 游戏自带的 `sdk` 模块本身就实现了 `nn::os` 等纯用户态服务**，
宿主只需提供 `svc`；它在加载全部模块后删掉了 136 条 `nn::os` 的 HLE。

- **当前状态**：本分支**未复制**该项目的任何源代码，仅参考其公开说明与目录结构。
- **若后续移植其文件**：将在本节逐项登记「文件名 / 来源仓库与提交号 / 许可证 / 本分支改动」，
  并在文件头保留原始版权声明（MIT 要求）。
- **同样致谢**：该项目 README 中列出的 Ryujinx、Atmosphère/mesosphère、yuzu、LibHac、
  SPIRV-Cross、deko3d 等，是我们理解 Horizon 语义与 Maxwell 着色器时的重要参考（各自许可证独立）。

## 许可证

- 上游代码：MIT License，全文见仓库根目录 [`LICENSE`](LICENSE)。
- 本分支新增/修改部分：同样以 **MIT License** 发布。
- 上游的版权声明与许可声明在所有副本中均予保留，符合 MIT 许可证的要求。
- 第三方代码各自的许可证见 [`docs/DEPENDENCIES.zh-CN.md`](docs/DEPENDENCIES.zh-CN.md)。

## 免责与合规声明

- **你必须拥有正版游戏**，包括《暗黑破坏神 II：狱火重生》Nintendo Switch 版本体
  与「术士君临」资料片（版本 1.0.34.0）。
- 本项目是一个**独立的非商业项目**，与暴雪娱乐、任天堂、华为**均无关联**，
  也未获其认可或赞助。
- Diablo 与 Blizzard Entertainment 是暴雪娱乐的商标。
- Nintendo Switch 是任天堂的商标。
- HarmonyOS 是华为的商标。
- 本项目**不含、也不分发任何游戏内容**。仓库中没有游戏程序、美术、文字、
  数据表或着色器；这些都需要使用者用自己的游戏文件在本地生成。
- 请**只配合你自己拥有的游戏使用**。
