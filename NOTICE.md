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
