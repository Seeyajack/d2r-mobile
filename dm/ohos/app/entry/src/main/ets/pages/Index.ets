/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// 真机探针页：装载机制实验 + 图形层能力探测 + 阶段 1/2/3 各小步。报告直接显示出来，也可以从 hilog 里取。
// 实验代码在 entry/src/main/cpp/dm_probe.c，说明见 dm/ohos/装载方案-镜像嵌入与定位.md 第四节。
// 能力探测在 entry/src/main/cpp/vk_probe.c，说明见 dm/ohos/图形层能力探测方案.md。
// 图形层阶段 1/2/3 在 dm/ohos/vk_backend.c，说明见 dm/ohos/图形层实施方案-Vulkan.md。
//
// 布局：**按钮区与报告区各自可滚动、各占一半**。按钮已有 10 个，早先单列布局会把报告挤出屏幕
// （真机上只能从 hilog 读），所以这里把两块都做成可滚动。
import dmprobe from 'libdmprobe.so';
import common from '@ohos.app.ability.common';

const HINT: string = '两个探针，点按钮就跑。\n\n' +
  '【装载机制实验】C 自定义 ELF 段能否生成 __start_/__stop_ 符号（预期：能）\n' +
  'B  mmap 自己的 .so 能否可执行（预期：成功，整个方案的基石）\n' +
  '   B-3 换应用数据目录的文件再试，分辨禁令范围\n' +
  'A  匿名内存能否拿到可执行权限（预期：失败）\n\n' +
  '【图形层能力探测】枚举设备的 Vulkan 能力，回答"Metal → Vulkan 这条路能不能走"。\n' +
  '不问窗口、不用 XComponent，纯枚举。最关键的一项是压缩贴图格式。\n\n' +
  '跑完把报告发回来。';

const VK_HINT: string = '图形层能力探测：只枚举能力，不创建窗口。\n' +
  '重点看三处：压缩贴图（BC/ASTC）、内存模型（统一内存）、时间线信号量与缓冲设备地址。';

const HOSTTEST_HINT: string = '上游宿主单测（dm/host/tests）：这批测试测的是**行为**，' +
  '能验掉垫片的哈希/时钟/内存读取对不对。每个单测是一个独立 .so，框架按名字载入后调用。\n' +
  '⚠️ 有的单测会让进程直接死掉（它们用 assert，assert 触发就是 abort，App 里没法隔到子进程）。' +
  '所以这个是**可续跑**的：崩了再点一次，它会跳过已出结果的、继续往后跑。\n' +
  '报告同时写一份到 filesDir/host_tests.log。';

@Entry
@Component
struct Index {
  @State result: string = HINT;
  // 单测筛选：留空 = 续跑；填 * = 清日志重头跑；填单测名（如 t_i18n）= 只跑那一个。查问题时用。
  @State testFilter: string = '';

  build() {
    Column({ space: 8 }) {
      Text('D2R Mobile · 鸿蒙移植 · 真机探针')
        .fontSize(16)
        .fontWeight(FontWeight.Bold)

      // 出图用的表面（阶段 4）：ArkUI 把 OHNativeWindow 交给 libdmprobe，原生侧再建 VkSurfaceKHR。
      // 不点"出图"按钮时它只是一块空白区域。
      XComponent({ id: 'dmvk', type: XComponentType.SURFACE, libraryname: 'dmprobe' })
        .width('100%')
        .height(220)

      // ---- 按钮区（可滚动）----
      Scroll() {
        Column({ space: 8 }) {
          Button('运行装载机制实验（A / B / C）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runProbes(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('探测图形层能力（Vulkan）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkProbe(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Text(VK_HINT)
            .fontSize(11)
            .textAlign(TextAlign.Start)
            .width('100%')
            .padding(4)

          Button('Vulkan 冒烟（阶段1：离屏清屏+读回）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkSmoke(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('SPIR-V 直通（阶段2：着色器画三角形）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkShader(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('内存池零拷贝（阶段3 前置：规模+机制）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkMemProbe(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('池零拷贝原型（3.1-A：池切片+设备地址）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkPoolProto(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('客体堆预留（3.1-B：4GB + 池切片）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkHeapReserve(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('池模块（3.1-C：登记+反查+搬运）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkPoolMod(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('回放最小闭环（3.2：命令流→出图）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkReplay(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('管线缓存落盘（冷/热对照）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkPsoCache(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('出图（3.3-A：XComponent→swapchain→上屏）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkOnScreen(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('可变帧率节拍（3.3-B：DisplaySoloist 5 组对照）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runVkPace(ctx.filesDir);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          TextInput({ text: this.testFilter, placeholder: '单测筛选：留空=续跑，*=重头跑，或填单测名' })
            .width('100%')
            .onChange((v: string) => { this.testFilter = v; })

          Button('运行上游宿主单测（续跑）')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runHostTests(ctx.filesDir, this.testFilter);
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Button('清日志、重头跑一遍')
            .width('100%')
            .onClick(() => {
              try {
                const ctx = getContext(this) as common.UIAbilityContext;
                this.result = dmprobe.runHostTests(ctx.filesDir, '*');
              } catch (err) {
                this.result = '调用原生模块失败：' + JSON.stringify(err);
              }
            })

          Text(HOSTTEST_HINT)
            .fontSize(11)
            .textAlign(TextAlign.Start)
            .width('100%')
            .padding(4)
        }
        .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.Top)

      // ---- 报告区（可滚动）----
      Scroll() {
        Text(this.result)
          .fontSize(12)
          .textAlign(TextAlign.Start)
          .width('100%')
      }
      .layoutWeight(1)
      .width('100%')
      .align(Alignment.TopStart)
      .padding(8)
      .backgroundColor('#F5F5F5')
      .borderRadius(8)
    }
    .padding(12)
    .width('100%')
    .height('100%')
  }
}
