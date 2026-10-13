# dm/ohos —— HarmonyOS NEXT 前端（设计骨架）

> 状态：**未编译、未在真机运行**。这是"缝"的设计落地，不是可用的实现。
> 依据：`适配清单-宿主层.md` ｜ 决策：鸿蒙侧另开接口（不动 `front.h`）+ 新建本目录

---

## 这个目录是什么

上游把"平台差异"收在一个只有 47 行的契约里：`dm/host/front.h`。

| 平台 | 契约的实现 |
|---|---|
| macOS | `dm/host/front_mac.m` |
| iOS | `dm/ios/Sources/front_ios.m` |
| **HarmonyOS NEXT** | **本目录的 `front_ohos.c`** ← 我们要写的 |

**所以这个目录的作用，就是"把 macOS/iOS 那一侧换成鸿蒙那一侧"。** 宿主（`dm/host/`）和图形层（`dm/gfx/`）的其余部分不用跟着分叉。

---

## 契约要求实现什么

### 必须实现（5 个，来自 `front.h`）

| 函数 | 鸿蒙侧怎么做 | 本骨架状态 |
|---|---|---|
| `front_run(guest)` | 等 XComponent 表面就绪，把游戏放到独立 pthread（64 MiB 栈）上跑 | ✅ 已写 |
| `front_metal_layer()` | **返回 NULL**（鸿蒙没有 Metal） | ✅ 已写 |
| `front_game_res(docked)` | 纯算术，**原样照抄** `front_mac.m` | ✅ 已写 |
| `front_text_input(...)` | 调 ArkTS 侧对话框（需要 NAPI 回调） | ⚠️ 占位，待接 |
| `front_save_failed(err)` | 先用弱实现（只记日志） | ⚠️ 占位 |

### 需要调用（2 个，实现在宿主里）

| 函数 | 实现在 | 本骨架状态 |
|---|---|---|
| `dm_host_preload_start(argc, argv)` | `dm/host/main.c` | ✅ 在 `dm_ohos_boot()` 里调 |
| `dm_touch_set(t, n)` | `dm/host/hle_touch.c` | ✅ 在触摸回调里调 |

### 鸿蒙专属新增（1 个）

| 函数 | 用途 |
|---|---|
| `front_ohos_native_window()` | 把 XComponent 的 `OHNativeWindow` 交给图形后端，由它创建 `VkSurfaceKHR` |

---

## 数据流

```
ArkTS 侧
  XComponent({ type: 'surface', libraryname: 'dmohos' })
        │
        ▼  (NAPI 加载 libdmohos.so)
  原生侧 Init()
        │  取 OH_NATIVE_XCOMPONENT_OBJ → napi_unwrap → OH_NativeXComponent*
        │  OH_NativeXComponent_RegisterCallback(...)
        ▼
  OnSurfaceCreated(component, window)
        │  window 就是 OHNativeWindow*（官方文档明确）
        │  → 存起来，给图形后端用
        ▼
  ArkTS 调 boot(romfs, save, temp)
        │  设环境变量 → dm_host_preload_start() → front_run(guest)
        ▼
  游戏跑在独立 pthread 上；触摸事件经 DispatchTouchEvent → dm_touch_set()
```

---

## 已核实的官方接口（来源：华为官方文档）

- 头文件：`<ace/xcomponent/native_interface_xcomponent.h>`、`<napi/native_api.h>`
- 回调结构体 `OH_NativeXComponent_Callback`，含 4 个回调：
  `OnSurfaceCreated` / `OnSurfaceChanged` / `OnSurfaceDestroyed` / `DispatchTouchEvent`
  —— 签名均为 `void (*)(OH_NativeXComponent* component, void* window)`，`window` 即 `OHNativeWindow` 句柄
- 注册：`OH_NativeXComponent_RegisterCallback(component, &callback)`
- 从 NAPI 取实例：`napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &v)` + `napi_unwrap(...)`
- 触摸：`OH_NativeXComponent_GetTouchEvent(component, window, &touchEvent)`
  —— `OH_NativeXComponent_TouchEvent` 含 `numPoints`、`type`、`touchPoints[]`（每点有 `id`、`x`、`y`、`isPressed`）
- 尺寸：`OH_NativeXComponent_GetXComponentSize(component, window, &w, &h)`
- ArkTS 侧：`XComponent({ id, type: 'surface', libraryname: 'dmohos' })`

**官方明确的生命周期约束**：`OnSurfaceCreated` 拿到的 `OHNativeWindow` 由系统持有一次引用，`OnSurfaceDestroyed` 之后引用减一、可能被释放。**所以销毁后必须让 `front_ohos_native_window()` 返回 NULL，图形后端也不得继续持有旧指针。**

---

## 已解决 / 未解决

### 已解决（读完 `dm/host/main.c` 后确认，2026-10-13）

| 问题 | 结论 |
|---|---|
| **怎么进入游戏主流程** | ✅ **不需要改 `main.c`**。它的 `main()` 不是 `static`，而且只做三件事：预载 → `front_run(guest_main)` → `return 0`。本文件的 `front_run` 不阻塞，所以鸿蒙侧直接 `extern int main(...)` 调它即可。 |
| `argc/argv` 怎么给 | ✅ 形状照 `main.c`：`argv[0]` 程序名、**`argv[1]` = 模块目录**（`loader.c` 的 `read_layout` / `register_gates` 从这里读 `*.fixups.tsv` / `*.gates.tsv`）、其后是模块名（缺省 `main` `sdk`）、`--` 之后是游戏参数。`dm_ohos_boot()` 已按此构造。 |
| `main()` 里的 `exit()` | ✅ `main()` 结尾是 `return 0`，**不调 `exit()`**，不会杀掉应用进程。 |

### 未解决

| 问题 | 说明 |
|---|---|
| **嵌入镜像的定位** | iOS 靠 `getsectiondata` 从 Mach-O 段里取游戏镜像（`loader.c` 有两套 `map_image`：嵌入版 / 从目录读 `*.patched.bin` 版）。鸿蒙没有 Mach-O 这套机制，要换一种嵌入与定位方式。**这是目前最大的未决点。** |
| `TargetConditionals.h` | `main.c` 第 170/173 行引用了它，鸿蒙上没这个头 —— 按适配清单第 1 项处理。 |
| `sysctl(KERN_PROC_PID)` | `main.c` 的 `proc_start_mono_ns()` 用它算进程启动时刻（启动时间线的零点）；鸿蒙上要换等价做法。 |
| 日志 | 上游用 `LOG()` 宏；鸿蒙侧要接到 hilog。 |
| 大栈线程 | `front_mac.m` 用 64 MiB 栈；鸿蒙侧 `pthread_attr_setstacksize` 是否允许这么大，需实测。 |

### 已用真实 SDK 核对通过的部分

骨架的 include 路径与 API 签名，已逐条对过本机安装的 DevEco Studio SDK
（`D:/Program Files/Huawei/DevEco Studio/sdk/default/openharmony/native/sysroot/usr/include/`）：

- `<ace/xcomponent/native_interface_xcomponent.h>`、`<napi/native_api.h>`、`<hilog/log.h>` 均存在
- `OH_NATIVE_XCOMPONENT_OBJ` = `"__NATIVE_XCOMPONENT_OBJ__"`
- `OH_NATIVEXCOMPONENT_RESULT_SUCCESS = 0`
- `OH_NativeXComponent_Callback` 的 `OnSurfaceCreated(OH_NativeXComponent*, void* window)`
- `OH_NativeXComponent_GetXComponentSize` / `GetTouchEvent` / `RegisterCallback`
- `OH_NativeXComponent_TouchEvent`：`numPoints`、`type`、`touchPoints[]`
- `OH_NativeXComponent_TouchPoint`：`id`、`x`、`y`、`isPressed`
- 触摸类型枚举：`OH_NATIVEXCOMPONENT_DOWN/UP/MOVE/CANCEL/UNKNOWN`

---

## 下一步

1. 解决"怎么进入游戏主流程"（上面第一条）——**这是能不能跑起来的关键**
2. 逐类替换适配清单里的 8 项苹果专属依赖
3. 在 DevEco Studio 里建工程，把 `libdmohos.so` 编出来（阶段 1：先出清屏画面）
