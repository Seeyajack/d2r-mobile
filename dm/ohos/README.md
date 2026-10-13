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

## 未解决 / 需要真机验证的问题

| 问题 | 说明 |
|---|---|
| **怎么进入游戏主流程** | `main.c` 的 `main()` 是进程入口；鸿蒙应用没有 `main()`。要么给 `main.c` 加一个可导出的入口，要么在鸿蒙侧复刻它的 boot 序列。**这是本骨架最大的未决点。** |
| `argc/argv` 怎么给 | `dm_host_preload_start(argc, argv)` 要参数，鸿蒙侧没有命令行。先构造一个最小 argv。 |
| `main()` 里的 `exit()` | 若直接调 `main()`，它退出时会 `exit()`，在鸿蒙上会结束整个应用进程。需要改成可返回的形式。 |
| 日志 | 上游用 `LOG()` 宏；鸿蒙侧要接到 hilog。 |
| 大栈线程 | `front_mac.m` 用 64 MiB 栈；鸿蒙侧 `pthread_attr_setstacksize` 是否允许这么大，需实测。 |
| `mach-o/*` 那类 | 宿主里定位嵌入镜像段的部分（见适配清单），要换机制。 |

---

## 下一步

1. 解决"怎么进入游戏主流程"（上面第一条）——**这是能不能跑起来的关键**
2. 逐类替换适配清单里的 8 项苹果专属依赖
3. 在 DevEco Studio 里建工程，把 `libdmohos.so` 编出来（阶段 1：先出清屏画面）
