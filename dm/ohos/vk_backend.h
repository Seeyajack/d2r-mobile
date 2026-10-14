// 离屏 Vulkan 冒烟后端 —— 对标上游 dm/gfx/metal/mtl_backend.h 的接口形状
// （init / shutdown / target / clear / readback；三角形在阶段 2 与 SPIR-V 直通一起做，
//  因为 NDK 没有 shaderc，着色器编译放电脑侧）。
// Offscreen Vulkan smoke backend, mirroring the shape of upstream dm/gfx/metal/mtl_backend.h
// (init / shutdown / target / clear / readback; the triangle lands in phase 2 with the direct-SPIR-V
// path, since the NDK ships no shaderc and shaders compile on the PC).
//
// 为什么先做这个：在写 6166 行回放层之前，先用最小代价证明
//   ① 鸿蒙侧 Vulkan 能初始化、能分配显存、能提交命令、能读回像素；
//   ② 探针 App 里的"清屏→读回→逐像素验证"给后面每一阶段提供同一套验收手段。
// 这正是 mtl_backend 在苹果侧扮演的角色（配 test_offscreen.c）。
// Why first: before the 6166-line replay layer, prove cheaply that Vulkan on HarmonyOS can initialise,
// allocate device memory, submit commands and read pixels back -- mtl_backend's role on Apple.
//
// 设计取舍：只用 **transfer** 通道（ClearColorImage + CopyImageToBuffer），
// 不碰渲染管线/着色器/渲染通道——把阶段 1 的变量降到最少。
// Design: transfer operations only (ClearColorImage + CopyImageToBuffer); no pipelines, no shaders, no
// render passes -- phase 1 keeps the variable count as low as possible.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct vk_ctx vk_ctx;
typedef struct vk_rt  vk_rt;

// 初始化：失败返回 NULL，err 里是原因（含 Vulkan 错误码与设备名）
vk_ctx *vk_backend_init(char *err, size_t errcap);
void    vk_backend_shutdown(vk_ctx *ctx);

// 建一个 RGBA8 离屏目标（阶段 1 只支持这一种格式）
vk_rt  *vk_backend_target(vk_ctx *ctx, uint32_t w, uint32_t h, char *err, size_t errcap);
void    vk_backend_target_free(vk_ctx *ctx, vk_rt *rt);

// 清成指定颜色；0 = 成功，非 0 = 失败
int     vk_backend_clear(vk_ctx *ctx, vk_rt *rt, float r, float g, float b, float a);

// 把目标读回内存（RGBA8，按行紧密）；返回写入字节数，<0 = 失败
long    vk_backend_readback(vk_ctx *ctx, vk_rt *rt, void *buf, size_t cap);

// 冒烟自测：init → 64×64 清红 → 读回 → 逐像素验证。
// 报告写进 out（设备名/驱动/API 版本/耗时/逐项结论），含结尾 0。
// 返回 0 = 全对；非 0 = 有问题（细节在报告里）。
int     vk_backend_smoke(char *out, size_t cap);

// 阶段 2：**SPIR-V 直通**——加载手写 SPIR-V 的顶点/片元着色器，建图形管线画一个三角形，
// 读回验证"三角形内 = 着色器颜色、三角形外 = 清屏色"。
// 证明"游戏着色器同源的语言（SPIR-V）不经翻译直接进 Vulkan"——上游离线链 Maxwell→SPIR-V 的
// 产物可以直接喂给 vkCreateShaderModule，Metal 那条路里的 spirv-cross/MSL/metallib 三个环节整段跳过。
// Phase 2: load hand-assembled SPIR-V, build a graphics pipeline, draw a triangle and verify pixels --
// proving SPIR-V (what the offline chain produces) feeds Vulkan directly, skipping MSL entirely.
int     vk_backend_shader(char *out, size_t cap);

// 阶段 3 前置：**内存池零拷贝的平台能力验证**。上游用 newBufferWithBytesNoCopy 把游戏内存池包成
// 零拷贝缓冲；本机没有 VK_EXT_external_memory_host，所以走"宿主自己分配"那条路——让客体堆的
// CPU 映射本身就是 Vulkan 内存的映射。本探针在真机上验这条路的两个前提：
//   ① 规模：能否一次分配并映射「客体堆量级」（约 3.7 GiB）的 HOST_VISIBLE|DEVICE_LOCAL 内存；
//   ② 机制：CPU 写进去的数据，GPU 经缓冲是否真的看得到（CPU 写 → GPU copy → CPU 读，逐字节比对）。
// 报告写进 out（含结尾 0）；返回 0 = 全过，非 0 = 有问题（细节在报告里）。
// Phase 3 prerequisite: verify the two premises of the "host allocates" zero-copy route on a real
// device -- (1) can a guest-heap-sized (~3.7 GiB) HOST_VISIBLE|DEVICE_LOCAL allocation be mapped,
// (2) does the GPU really see what the CPU wrote (CPU write -> GPU copy -> CPU read, byte compared).
int     vk_backend_memprobe(char *out, size_t cap);

// 阶段 3 第一步原型（3.1-A）：**池零拷贝**。造一块"模拟客体堆"（Vulkan 分配 + 映射），从中切两个池
// 各建 `VkBuffer` 绑到池偏移上（对标 `nvn_mtl.m` 的 `mtl_pool_add`），然后验三件事：
//   ① 缓冲对齐要求（池偏移的约束）；② 缓冲设备地址的线性不变式（GPU−CPU 是常数，`mtl_gpu_addr` 靠它）；
//   ③ CPU 写池 A → GPU copy → CPU 读池 B 逐字节一致（池切片真的能被 GPU 读写）。
// 全部在探针内，不动任何上游文件。返回 0 = 全过。
// Phase 3 step 1 prototype: the pool zero-copy path, entirely inside the probe (no upstream changes).
int     vk_backend_poolproto(char *out, size_t cap);
