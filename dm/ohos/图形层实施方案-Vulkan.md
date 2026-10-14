# 图形层实施方案 —— NVN → Vulkan

> 编制：2026-10-14 ｜ 状态：**方案待批准**
> 对象：`dm/gfx/`（上游 15816 行图形层）→ 鸿蒙
> 前置：能力探测已过止损点（`dm/ohos/图形层能力探测方案.md`）

---

## 一、上游图形层的结构（勘测结论）

| 块 | 规模 | 是什么 | 对鸿蒙意味着什么 |
|---|---|---|---|
| `nvn/hle_nvn.c` | 1202 行 | **接口层**：接管 NVN 的 523 个符号（216 个有实现），把每次调用**录进游戏提供的命令内存**；对象就地存在游戏给的内存里（`nvn_obj.h`） | **平台中立，基本原样复用** |
| `nvn/nvn_mtl.m` | **6166 行** | **回放层**：提交时把记录流回放成 **Metal** 命令 | **这就是要重写的东西** → `nvn_vk.c` |
| `nvn/nvn_thunks.S` | 6813 行（**生成文件**） | 523 个跳板：计数 + 跳实现 | arm64 汇编，两平台调用约定相同，预计小改即可（`nvn_entry.S` 已有 `CSYM` 分叉先例） |
| `metal/mtl_backend.*` | 471 行 | **无窗口的离屏测试后端**（init/target/clear/triangle/readback 六个函数），配 `test_offscreen.c` | **鸿蒙照做一个 `vk_backend`**——这正是第一阶段 |
| `tex/` | 688 行 | 贴图：bcdec 解码（**上游自带**）+ Tegra 块线性解排布 + Metal 格式映射 | 解码/解排布平台中立；格式映射要出 Vulkan 版 |
| `tests/` | 1825 行 | 图形层自己的测试 | 随单测链自动收编 |
| `*.tsv` 表 | — | 着色器映射、管线清单、槽位表 | 链是 L2，表的内容偏 L3（游戏派生） |

## 二、决定性的好消息：**着色器可以 SPIR-V 直通**

上游的离线翻译链是：

```
Switch Maxwell 机器码 → (Ryujinx 翻译库，C# 工具) → SPIR-V → spirv-cross → MSL → metallib
```

**SPIR-V 恰好是 Vulkan 的原生输入。** 也就是说鸿蒙侧：
- 离线链的前半段（Maxwell → SPIR-V）**原样复用**；
- 后半段（spirv-cross 转 MSL、xcrun 编 metallib）**整段跳过**——SPIR-V 直接 `vkCreateShaderModule`；
- **少了一个工具依赖、少了一个翻译环节**（ Metal 那条路是为了迁就 Metal 才多走的）。

## 三、分阶段实施（每阶段都有真机可验证的交付物）

| 阶段 | 内容 | 交付物（怎么算完成） | 依赖 |
|---|---|---|---|
| **✅ 1. Vulkan 冒烟（无窗口离屏）**——真机全通过（清屏+读回逐像素验证） | 照 `mtl_backend.h` 的六个函数做一个 `vk_backend`：init / shutdown / target / clear / triangle / **readback**；用 NDK 的 `libvulkan.so` | 探针 App 加"Vulkan 冒烟"按钮：清屏→画三角形→**读回像素**验证颜色正确。**不依赖游戏文件、不依赖窗口** | 已有（vk_probe 已证明设备能初始化 Vulkan） |
| **✅ 2. SPIR-V 直通**——真机全通过（glslang 产物画三角形，75ms；手写 SPIR-V 会让驱动崩渲染服务，见下） | 在 vk_backend 里加载一个 SPIR-V 着色器画同一个三角形 | 同上，但三角形是**游戏着色器同源的语言**画的 ⇒ 证明"游戏着色器不经翻译直接进 Vulkan" | 阶段 1 |
| **⬜ 3. 回放层 `nvn_vk.c`（大头，当前任务）** | 按 `nvn_obj.h` 的记录格式回放成 Vulkan。分小步：内存池零拷贝（**前提已验，见下**）→ 管线/PSO 哈希缓存 → 绘制 → 同步对象/事件/计数器 → 贴图上传（含 **BC→ASTC**，解码用上游自带的 `tex/nvn_bcdec.c`） | 每小步配一条真机可验证的检查；最终目标：`dm/gfx/tests/` 的测试在真机通过 | 阶段 2 |
| ⬜ 4. 出图与节拍 | XComponent surface + swapchain（或离屏 blit）；`nvn_pace.h` 节拍器的鸿蒙等价物 | 屏幕上出图 | 阶段 3 |
| ⬜ 5. 接真游戏 | L3 | 游戏跑起来 | 游戏文件 |

### 阶段 3 第一步（内存池零拷贝）——前提已验（2026-10-14 晚，真机）

探针 `vk_backend_memprobe`（App 按钮「内存池零拷贝（阶段3 前置：规模+机制）」）在 Mate 70 Pro+ 上跑通，**0 项失败**：

| 项 | 读数 |
|---|---|
| 内存堆 | 堆 0 = **15289 MiB**（显存）、堆 1 = 640 MiB |
| 候选类型 | 类型 0 = `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT`（属性 `0x7`）——**一致内存，不需要 flush** |
| ① 规模 | 64 MiB / 256 MiB / 1 GiB / 2 GiB / **3.7 GiB** 逐个"分配 + 映射 + 首中尾读写"成功（3.7 GiB 用 **1729 ms**） |
| ② 机制 | 同块内存上 A(偏移 0)/B(偏移 65536)，CPU 写 A → `vkCmdCopyBuffer` → CPU 读 B，**65536 字节逐字节一致** |
| 缓冲对齐 | `vkGetBufferMemoryRequirements` 给出 **alignment = 64**（池切片偏移要按它对） |

**结论（阶段 3 可直接用）**：

1. **零拷贝路线成立**：走"宿主自己分配"——`vkAllocateMemory`（类型 0）+ `vkMapMemory`，
   **用该地址当客体堆 `g_heap`**；`mtl_pool_add` 的对应物按 `off = mem − g_heap` 建
   `VkBuffer` + `vkBindBufferMemory`（对标 Metal 的 `newBufferWithBytesNoCopy`）。
   **不需要 `VK_EXT_external_memory_host`**。
2. 选中的类型 **`HOST_COHERENT`** ⇒ 不需要 flush/invalidate ⇒ `hle_nvn.c` 的
   `nvnMemoryPoolFlushMappedRange` 空实现**可以继续空着**。
3. **池切片绑定非零偏移可行**（实测偏移 65536），但偏移须满足缓冲的 `alignment`。

> 诚实边界：3.7 GiB 是在**没有其它图形资源共存**时分配的。堆 0 有 15 GiB、余量充足，
> 但真接入游戏后要与其它分配共存，届时需复看。

#### 3.1-A 池零拷贝原型 —— 真机通过（2026-10-14 晚）

探针 `vk_backend_poolproto`（App 按钮「池零拷贝原型（3.1-A：池切片+设备地址）」）：
造一块 32 MiB「模拟客体堆」，从中切两个池、各建 `VkBuffer` 绑到池偏移上，**0 项失败、总耗时 39 ms**。

| 项 | 读数 |
|---|---|
| 缓冲对齐要求 | **alignment = 256**（64 KiB / 1 MiB / 16 MiB 三种大小一致；用量含 `STORAGE_BUFFER` + `SHADER_DEVICE_ADDRESS`） |
| 池切片绑定 | 池A 偏移 0、池B 偏移 2 MiB，均绑定成功（**非零偏移可行**） |
| 线性不变式 | 两个池的 **GPU − CPU = 0**（完全恒等）⇒ `mtl_gpu_addr` 的 `gpu = pool.gpu + off` 成立 |
| GPU 实际读写 | CPU 写池 A → `vkCmdCopyBuffer` A→B → CPU 读池 B，**262144 字节逐字节一致**（4 ms） |

**两条要记的**：

1. **对齐要求随用量变**：只带 `TRANSFER_*` 时是 64，加上 `STORAGE_BUFFER` / `SHADER_DEVICE_ADDRESS`
   后是 **256**。池偏移必须按"实际用量查出来的 `alignment`"对齐（游戏的分配器给的是页对齐，天然满足）。
2. 本设备上 `GPU − CPU` **恰好是 0**，但**不能把它当常量依赖**——实现里仍按"同一堆内差值为常数"
   这个不变式做反查（上游 `mtl_gpu_addr` 就是这么写的）。

## 四、鸿蒙 NEXT 特有游戏机制的接入点（按阶段落位）

| 机制 | 是什么 | 落在哪个阶段 |
|---|---|---|
| `VK_OHOS_surface` + `OHNativeWindow` | **已证实存在**（NDK 的 `vulkan_ohos.h`，`VkSurfaceCreateInfoOHOS.window` 直接收 `OHNativeWindow*`）⇒ 阶段 4 走**标准 swapchain**，不需要离屏 blit 的退路 | 4 |
| `IMPORT_NATIVE_BUFFER_INFO_OHOS` | `OH_NativeBuffer` 可**导入为 Vulkan 内存/图像** ⇒ 游戏内存池与显示的零拷贝候选（与"统一内存零拷贝"方案互补） | 3（池内存）/4（出图） |
| `OH_DisplaySoloist`（可变帧率自渲染调度） | 自渲染视区的帧率控制，可按负载设定帧率 ⇒ `nvn_pace.h` 节拍器的**鸿蒙等价物**，且比固定 vsync 强（低负载降帧省电） | 4 |
| 图形加速服务（超帧 / 稳态帧率） | AI 插帧与稳态帧率 ⇒ 对"60fps 跑稳"有直接价值；可能需要 AGC 配置/上架路径 | 后期调研项 |
| 游戏服务 / 性能调度 | 游戏模式的调度与资源保障 | 后期调研项 |

> 侦察证据：`$NDK/sysroot/usr/include/vulkan/vulkan_ohos.h`（`VkSurfaceCreateInfoOHOS.window: OHNativeWindow*`）、
> `vulkan_core.h` 的 `VK_STRUCTURE_TYPE_*_OHOS` 系列（surface / native buffer 导入 / swapchain image）。
> 另：NDK **没有** shaderc/glslang ⇒ 阶段 1 的三角形并入阶段 2（SPIR-V 直通一起验，编译在电脑侧解决）。

## 五、需要早查的三个前提（阶段 1 期间顺带查）

1. **OHOS 的 Vulkan WSI**：`libvulkan.so` 有没有 `VK_OHOS_surface` 扩展（决定阶段 4 是走标准 swapchain 还是离屏 blit 到 XComponent）；
2. **BC→ASTC 的取舍**：设备不支持 BC ⇒ 转码放离线（构建期）还是运行时（首载慢一点但不用改链）；
3. **`nvn_thunks.S` 的再生成**：生成器在哪、OHOS 侧要不要 `CSYM` 一类的宏（有 `nvn_entry.S` 的先例可抄）。

## 六、风险与诚实清单

- 阶段 3 是 6000 行级的重写，**必须分小步、每步真机可验**——这是把风险摊薄的唯一办法；
- `vertexPipelineStoresAndAtomics = 无`（能力探测发现的真限制）⇒ 上游着色器若在顶点阶段用存储缓冲/原子操作，需要绕法——**阶段 2 就能探到**（用真实 SPIR-V 试）；
- ~~池内存谁分配（设备无 `external_memory_host`）⇒ 若游戏自分配则"宿主零拷贝"不成立~~ →
  **已验（2026-10-14 晚）**：客体堆由宿主 `mmap` 预留，宿主说了算 ⇒ 零拷贝走"宿主自己分配"路，
  **不需要** `external_memory_host`。读数见第三节末。
- 本文的行数与判断基于静态勘测，动手后以实测为准。
