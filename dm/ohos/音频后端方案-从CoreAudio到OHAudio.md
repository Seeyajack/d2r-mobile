# 音频后端方案：从 CoreAudio AudioUnit 到鸿蒙 OHAudio

> 编制日期：2026-10-14 ｜ **状态：第一步已完成并通过验证；第二步待做**
> 依据：`dm/host/hle_audio.c`（253 行，逐行读完）+ 鸿蒙 NDK 官方头文件（`sysroot/usr/include/ohaudio/`，逐条核实）
> 配套：`dm/ohos/compat/README.md`

---

## 一、结论先行

**好消息比预期大：这块工作量小，而且不会挡住"游戏跑起来"。**

两句话讲清为什么：

1. `hle_audio.c` 那 253 行里，**真正与苹果绑定的只有 3 处**（建流、启动/停止、释放）。
   剩下的一整套——待播队列、已释放队列、按真实时间消耗的"静音接收端"、事件通知、缓冲追加/取回——
   **全部是平台无关的**，一行都不用改。

2. 那个"静音接收端"**压根不用任何平台音频接口**（只用 `usleep` + 宿主的单调时钟）。
   而"要不要真出声"这个开关（`real_audio()`）判断的正是 `TARGET_OS_IPHONE`——
   垫片一上它就是 0，**鸿蒙侧默认就走静音接收端**。

   所以：**只要 `hle_audio.c` 能编过，音频的节奏就是对的**（不出声，但游戏的音频线程不会卡住）。
   真出声是**增量**，不是前置条件。

**也就是说，这块可以切成两步，第一步几乎没有技术风险。**

---

## 二、现状解剖：哪块能留、哪块必须换

`dm/host/hle_audio.c` 逐段看（行号可对回原文）：

| 行号 | 内容 | 处置 |
|---|---|---|
| 16-40 | 常量、`aout` 结构（队列 / 已释放环 / 原子变量）、打开登记表 | ✅ 留。**只有第 29 行的 `AudioUnit au;` 要换类型** |
| 43-62 | `notify_release()` + `event_notify()`：缓冲释放后通知客体 SystemEvent 的线程 | ✅ 留（它用的信号量正是本轮刚做的 dispatch 垫片） |
| 66-88 | **`pull()`**：从待播队列取指定字节数、整块播完就挪进已释放环 | ✅ 留，**这是整块的心脏，完全平台无关** |
| 90-91 | `g_render_calls` 计数（生命周期验证用） | ✅ 留 |
| **92-98** | **`au_render()`**：CoreAudio 的渲染回调 | 🔴 **换**（苹果专有签名） |
| **99-117** | **`au_open()`**：建并配置 AudioUnit | 🔴 **换** |
| 120-128 | `null_sink()`：静音时按 48 kHz 每 5 ms 实时消耗 | ✅ 留，**零平台依赖** |
| 129 | `real_audio()`：`DM_AUDIO` 环境变量 + `TARGET_OS_IPHONE` 缺省 | ✅ 留（垫片让鸿蒙侧缺省为 0 = 静音） |
| 133-227 | 15 条 `HLE_FORCE`（参数默认值、设备列表、开关、查询、缓冲操作） | ✅ 留，**只有内部调 `au_open` / `AudioOutputUnitStart/Stop` 的那几行要收口** |
| 233-253 | `dm_audio_suspend()`：切后台暂停、期间由静音接收端接手 | ✅ 逻辑留，**里面两处 AudioUnit 调用要收口** |

**结论：改动集中在 3 个函数的替换 + 4 个调用点的收口。**

---

## 三、官方接口（已核实，附出处）

来源：`D:\Program Files\Huawei\DevEco Studio\sdk\default\openharmony\native\sysroot\usr\include\ohaudio\`

### 3.1 建流（`native_audiostreambuilder.h`）

| 接口 | 出处行 | 说明 |
|---|---|---|
| `OH_AudioStreamBuilder_Create(&builder, type)` | 65 | `type = AUDIOSTREAM_TYPE_RENDERER`（=1，`native_audiostream_base.h:117`） |
| `SetSamplingRate(b, rate)` | 97 | 我们传 48000 |
| `SetChannelCount(b, n)` | 113 | 游戏要求几声道就报几（**风险点，见第五节**） |
| `SetSampleFormat(b, AUDIOSTREAM_SAMPLE_S16LE)` | 127 | 枚举值 =1（`base.h:144`） |
| `SetEncodingType(b, AUDIOSTREAM_ENCODING_TYPE_RAW)` | 142 | 枚举值 =0（`base.h:177`）——我们要的就是裸 PCM |
| `SetLatencyMode(b, AUDIOSTREAM_LATENCY_MODE_NORMAL)` | 157 | 枚举值 =0（`base.h:329`） |
| `SetRendererInfo(b, usage)` | 189 | **只收 usage 一个参数**；用 `AUDIOSTREAM_USAGE_GAME`（=11，`base.h:297`） |
| `SetRendererWriteDataCallback(b, cb, userData)` | 408 | **API 12 起可用**（真机 API 26 ✓）。比旧的 `SetRendererCallback` 新，后者标了 deprecated since 20 |
| `GenerateRenderer(b, &renderer)` | 318 | |
| `Destroy(b)` | 81 | 生成 renderer 后即可销毁 builder |

### 3.2 播放控制（`native_audiorenderer.h`）

| 接口 | 行 | 对应苹果侧 |
|---|---|---|
| `OH_AudioRenderer_Start(r)` | 78 | `AudioOutputUnitStart` |
| `OH_AudioRenderer_Pause(r)` | 92 | 暂停（保留缓冲） |
| `OH_AudioRenderer_Stop(r)` | 106 | `AudioOutputUnitStop` |
| `OH_AudioRenderer_Flush(r)` | 120 | 清缓冲 |
| `OH_AudioRenderer_Release(r)` | 64 | `AudioUnitUninitialize` + `AudioComponentInstanceDispose` |

### 3.3 回调签名（`native_audiostream_base.h`）

```c
typedef OH_AudioData_Callback_Result (*OH_AudioRenderer_OnWriteDataCallback)(
    OH_AudioRenderer* renderer, void* userData, void* audioData, int32_t audioDataSize);
```

（`base.h:904`。返回值 `AUDIO_DATA_CALLBACK_RESULT_VALID` = 0，`base.h:886`。）

**与苹果那条的关系**：苹果是"回调里往 `mData` 写 `mDataByteSize` 字节、返回 `noErr`"；
鸿蒙是"往 `audioData` 写 `audioDataSize` 字节、返回 VALID"。**参数含义一一对应**，
所以回调体里那句 `pull(a, 目标指针, 目标字节数)` **一模一样**。

### 3.4 声道布局（顺带核实）

`multimedia/native_audio_channel_layout.h` 里有成套布局常量（`CH_LAYOUT_MONO`/`STEREO`/`SURROUND`/`5POINT0`/`5POINT1`…），
`ohaudio` 的两个主头都会把它带进来。**若 6 声道需要显式布局，`SetChannelLayout` 可用**（API 12 起）。

---

## 四、替换映射表

| 苹果（现状） | 鸿蒙（拟） |
|---|---|
| `AudioUnit au;` | `void *au;`（指向我们的流句柄） |
| `au_render(ctx, flags, ts, bus, frames, bl)` | `dm_ao_render(r, ctx, audioData, audioDataSize)` |
| `au_open(a)`：`AudioComponentFindNext` + `InstanceNew` + 设格式 + 设回调 + `AudioUnitInitialize` | `dm_ao_open(ch, 48000)`：Builder 六步配置 + `SetRendererWriteDataCallback` + `GenerateRenderer` |
| `AudioOutputUnitStart(au)` | `dm_ao_start(au)` → `OH_AudioRenderer_Start` |
| `AudioOutputUnitStop(au)` | `dm_ao_stop(au)` → `OH_AudioRenderer_Stop` |
| `AudioUnitUninitialize` + `AudioComponentInstanceDispose` | `dm_ao_close(au)` → `OH_AudioRenderer_Release` |

**为什么要多一层 `dm_ao_*` 而不是直接在 `hle_audio.c` 里写 OHAudio**：
`hle_audio.c` 是上游文件，我们希望它的**阅读体验和改动量都最小**。
把鸿蒙那套东西关在一个新文件 `dm/ohos/audio_ohos.c` 里，
`hle_audio.c` 只多一行 `#if defined(__OHOS__)` 分支 + 4 个名字更好懂的调用。

---

## 五、三个必须先定的事（含风险）

### 风险 1：声道数——**这是本方案唯一真正的不确定点**

游戏通常按 **6 声道（5.1）** 开音频（原代码的注释里写着"常见 6"）。
但 OHAudio 的 `SetChannelCount` 文档**只写了"参数非法会报错"，没有写明上限**。

要命的地方在于：**我们不能偷偷把 6 改成 2**。因为游戏会调 `GetAudioOutChannelCount`
问"我给你几声道"，然后按那个数写 PCM。修改返回值会让游戏按错的方式写缓冲，声音会全乱。

所以只有两种结局：

| 结局 | 应对 |
|---|---|
| 鸿蒙能开 6 声道 | 直接照做，最省事 |
| 鸿蒙只能开 2 声道 | 宿主侧**自己做 6→2 下混**（在 `pull()` 之后加一层）。这是**新增的算法工作**，不是搬运——但它是标准做法，可做，只是要写要验 |

**判定方式**：真机上写个 10 行的自检，试着建 6 声道的流，看 `GenerateRenderer` 返回什么。
**这一步应该排在最前面做**，因为它决定后面是"照搬"还是"要写下混"。

### 风险 2：谁来决定"什么时候该出声"

苹果那边靠 `TARGET_OS_IPHONE` 缺省决定。鸿蒙侧垫片让它是 0（静音），
**这个缺省要不要改**？我的建议：**第一版不改**——保持"缺省静音、`DM_AUDIO=1` 才出声"，
与 Mac 侧一致。理由：调试阶段没人想被声音干扰，且静音接收端的节奏与真机相同，
不影响任何逻辑验证。

### 风险 3：音频焦点 / 打断（**建议先不做**）

鸿蒙有"音频焦点"概念（`native_audio_session_manager.h`），别的应用（来电、音乐）可以抢。
苹果那边对应的是 `AVAudioSession`，原代码没走这套，而是靠前端在切后台时调 `dm_audio_suspend(1)`。

**建议**：第一版**沿用现有钩子**，不引入焦点管理。理由是焦点属于打磨项，
且引入它会牵出"打断时游戏该暂停还是静音"这类产品决策，不适合现在摊开。
标为后续项。

---

## 六、建议分两步走

### 第一步（低风险，建议立刻做）：让它编过，静音节奏立刻可用

只做三件事：

1. 把 `#include <AudioToolbox/AudioToolbox.h>` 与 `au_render` / `au_open` 两段**加 `#if !defined(__OHOS__)` 守卫**；
2. `aout` 里的 `AudioUnit au` 换成按平台取类型；
3. 4 个调用点（启动/停止/释放、以及 `dm_audio_suspend` 里两处）收口成 `DM_AO_*` 宏，
   鸿蒙侧先实现成"不做事、返回成功"。

**结果**：`hle_audio.c` 编过 ⇒ **宿主 71/71 全部编过**；
鸿蒙上音频走静音接收端，**节奏与真机一致，游戏不会被音频卡住**。

这一步的意义不只是"又一个文件编过"——它是**第一次让宿主"完整"**，
之后才能整批链接成 `libdmohos.so`。

### 第二步（要实测，有上面风险 1）：真出声

新增 `dm/ohos/audio_ohos.c`，把 `DM_AO_*` 换成真正的 OHAudio 调用。
先做**声道数探测自检**，再决定"直接照搬"还是"加下混层"。

---

## 七、改动清单

**新增**（都在 `dm/ohos/`，不碰上游）：

| 文件 | 内容 |
|---|---|
| `audio_ohos.h` | 5 个函数声明：`dm_ao_open / start / stop / close` + 回调类型 |
| `audio_ohos.c` | OHAudio 实现（第一步先给空实现，第二步填真实现） |

**改上游**（`dm/host/hle_audio.c`，全部 `#if defined(__OHOS__)` 分叉，苹果侧一字不变）：

| 位置 | 改动 |
|---|---|
| 第 10 行 | `AudioToolbox` 的 include 加守卫 |
| 第 29 行 | `AudioUnit au;` → 按平台取类型 |
| 第 92-117 行 | `au_render` / `au_open` 两段加守卫 |
| 第 168、177、189、242、244 行 | 收口成 `DM_AO_START/STOP/CLOSE` |
| `dm/ohos/CMakeLists.txt` | 加入 `audio_ohos.c`（连同 `libohaudio.so` 一起链接） |

---

## 八、待真机验证清单

1. **6 声道能否建流**（决定要不要写下混）——**最先做**。
2. `AUDIOSTREAM_USAGE_GAME` 这个用法是否影响混音策略（会不会独占音频）。
3. `OH_AudioRenderer_Stop` 与 `Pause` 在"切后台"场景下哪个更贴合原语义
   （原代码要的是"暂停发声但继续按实时消耗"，理论上两者都要配合静音接收端）。
4. `dm_audio_render_calls()`（声卡回调计数）在鸿蒙侧是否照常增长——
   它是**生命周期验证的观测点**，恢复后必须继续涨。
5. 回调线程与游戏线程的竞争：OHAudio 的回调跑在音频服务线程上，与 CoreAudio 一样属于"别阻塞"的线程。
   `pull()` 是纯内存操作 + 一次信号量 signal，**符合要求**，但要在真机上确认没有偶发卡顿。

---

## 九、第一步完成情况（2026-10-14）

**已做**，且**没有改动任何现有行为**：

| 项 | 结果 |
|---|---|
| `hle_audio.c` 编过 | ✅ |
| 全宿主编译探针 | ✅ **71 个 .c 通过 68，未通过 3** —— 剩下 3 个**全部**是等游戏文件生成数据表（`diag.c` / `dm_bright.c` / `dm_vault_ui.c`），与音频无关 |
| 收口是否干净（符号级实测） | ✅ `hle_audio.o` 对音频后端只认 `dm_ao_open/start/stop/close` 四个名字；**苹果音频符号一个都没漏出来**（`AudioUnit*` / `AudioOutputUnit*` / `AudioComponent*` / `noErr` 全部为空） |
| 新增文件的依赖 | ✅ `audio_ohos.o` 只提供那 4 个函数、只引用 `dm_log_printf` —— **第二步填实现时不必再碰 `hle_audio.c`** |

**当前鸿蒙侧的音频行为**：一律走静音接收端——**节奏与真机一致，只是没声音**。
若有人设了 `DM_AUDIO=1`，会在日志里看到一句明确说明（"真出声后端尚未实现（第一步）"），
**不会静默地什么都不做**。

### 一个要记住的副作用

`dm_audio_render_calls()`（声卡回调累计次数）**在鸿蒙侧目前恒为 0**。
原因是上游的设计里这个计数只在**真出声回调**里加（注释写明"静音接收端不计"），
而第一步没有真出声后端。第二步做完它就会开始增长。
它是生命周期验证的观测点之一，**现阶段不要拿它当判据**。

---

## 十、第二步（待做）

1. **先做声道数探测自检**（真机，10 行）：试着建 6 声道的流，看 `GenerateRenderer` 返回什么。
   这一步决定后面是"照搬"还是"要写下混"，**必须先做**。
2. 按第三节的接口把 `dm/ohos/audio_ohos.c` 的空实现填成真的：
   Builder 六步 → `SetRendererWriteDataCallback` → `GenerateRenderer`；
   回调里调 `pull()` 的对接函数（需要把 `pull` 通过一个函数指针暴露给 `audio_ohos.c`）。
3. 走第八节的待验证清单。

**设计上留了一个口子**：`audio_ohos.c` 需要拿到 `aout *` 才能调 `pull()`，
所以实现时会在 `audio_ohos.h` 里加一个"回调注册"入口，
由 `hle_audio.c` 在建流时把 `pull` 的桥接函数传进来——
**这样 `audio_ohos.c` 仍然不需要知道 `aout` 的内部结构**。
