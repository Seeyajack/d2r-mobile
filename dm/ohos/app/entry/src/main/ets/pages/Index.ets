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

// 第 1 站：跑三个真机实验并把报告显示出来。
// 实验代码在 entry/src/main/cpp/dm_probe.c，说明见 dm/ohos/装载方案-镜像嵌入与定位.md 第四节。
import dmprobe from 'libdmprobe.so';

const HINT: string = '点下面的按钮，在真机上跑三个实验。\n\n' +
  'A  匿名内存能否拿到可执行权限（预期：失败）\n' +
  'B  mmap 自己的 .so 能否可执行（预期：成功，整个方案的基石）\n' +
  'C  自定义 ELF 段能否生成 __start_/__stop_ 符号（预期：能）\n\n' +
  '跑完把报告发回来，就能定下装载方案。';

@Entry
@Component
struct Index {
  @State result: string = HINT;

  build() {
    Column({ space: 12 }) {
      Text('D2R Mobile · 鸿蒙移植 · 第 1 站')
        .fontSize(18)
        .fontWeight(FontWeight.Bold)

      Button('运行三个实验')
        .width('100%')
        .onClick(() => {
          try {
            this.result = dmprobe.runProbes();
          } catch (err) {
            this.result = '调用原生模块失败：' + JSON.stringify(err);
          }
        })

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
    .padding(16)
    .width('100%')
    .height('100%')
  }
}
