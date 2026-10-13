# dm/ohos —— HarmonyOS NEXT 前端（设计骨架）

> 状态：`front_ohos.c` **尚未编进 `libdmohos.so`**——因为它依赖的宿主还没替换完苹果专属依赖。
> 但本目录并非"未验证"：`map_image_ohos.c` 已通过真机实验验证，`app/` 工程已在真机 build/run 通过。
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
| `sysctl(KERN_PROC_PID)` | `main.c` 的 `proc_start_mono_ns()` 用它算进程启动时刻；鸿蒙上要换等价做法。 |
| 日志 | 上游用 `LOG()` 宏；鸿蒙侧要接到 hilog（`map_image_ohos.c` 里已用 `OH_LOG_Print` 示范）。 |
| 大栈线程 | `front_mac.m` 用 64 MiB 栈；鸿蒙侧 `pthread_attr_setstacksize` 是否允许这么大，需实测。 |

### 已解决

| 问题 | 结论 |
|---|---|
| 进入游戏主流程 | ✅ 直接 `extern int main(...)` 调 `main.c` 的 `main()`（它不调 exit，`front_run` 不阻塞） |
| `TargetConditionals.h` | ✅ `loader.c` 已加 `#if !defined(__OHOS__)` 守卫；未定义时 `#if TARGET_OS_IPHONE` 按 0 处理 |
| **嵌入镜像的定位** | ✅ 自定义 ELF 段 + `__start_/__stop_` 链接器符号（真机实验 C 验证）。实现：`map_image_ohos.c`，接线在 `loader.c` 的 `#elif defined(__OHOS__)` 分支 |
| **运行时映射机制** | ✅ 匿名 `mmap` + `memcpy` + `mprotect` 成 R+X（真机实验 A 验证）。**照抄上游桌面版 `map_image()`**，不需要 iOS 的文件映射 |
| **线程槽机制（`tsd_check`）** | ✅ 真机测出 musl 是**两级间接**：`槽 = *(void**)(tpidr_el0 − 272) + 键号×8`（实验 E/E2/F 验证）。`patch_nso.py` 已加 `DM_PATCH_TARGET` 平台开关，`tsd_check()` 已改。详见 `线程槽方案-musl改法.md` |

---

## 本目录文件

| 文件 | 是什么 |
|---|---|
| `front_ohos.c/.h` | `front.h` 契约的鸿蒙实现（前端骨架） |
| `map_image_ohos.c/.h` | 鸿蒙版 `map_image`：把嵌入的映像放进内存（**真机实验验证过**） |
| `CMakeLists.txt` | 鸿蒙 NDK 构建脚本（骨架，暂只挂前端） |
| `装载方案-镜像嵌入与定位.md` | 嵌入/定位/映射的完整方案 + 真机实验结果 |
| `app/` | 鸿蒙工程（devecocli create 生成，含第 1 站三个真机实验） |

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

## 下一步（2026-10-13 深夜更新）

之前那条"重新设计线程槽机制""解决怎么进入游戏主流程"的待办**已经完成**，不再重复。

现在的主线是**把宿主编进 `libdmohos.so`**，分四步：

1. **搭兼容垫片** `dm/ohos/compat/`——宿主里的苹果头全是 `#include <...>` 尖括号形式，
   用同名垫片头 + `-I` 前置即可替换，**上游文件一个字都不用改**（见该目录的 `README.md`）
2. **统一日志**：把上游散落的 `LOG()` 接到 hilog（`dm_log.h`）
3. **分批把宿主编进来**：批 1 平台中立 → 批 2 系统服务 → 批 3 平台相关
4. **图形层（Metal → Vulkan）暂不碰**——等宿主编译这条主线走完再开
