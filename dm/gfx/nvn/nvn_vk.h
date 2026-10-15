// 鸿蒙侧回放层（`nvn_vk.c`）—— 对标上游 `nvn_mtl.m`（Metal 回放层）。**第一步只有内存池这一块。**
// The HarmonyOS replay layer, the counterpart of upstream's nvn_mtl.m. Step 1 covers memory pools only.
//
// 为什么池排第一：游戏的顶点、贴图、渲染目标、命令内存**全都住在内存池里**。回放层要做的第一件事，
// 就是把"游戏给的一个地址"翻译成"GPU 能用的缓冲 + 偏移"——上游 `mtl_addr` / `mtl_gpu_addr` 干的就是这个。
// Why pools first: everything the game feeds the GPU lives in memory pools. The replay layer's first job
// is translating a guest address into a GPU buffer + offset -- exactly what mtl_addr / mtl_gpu_addr do.
//
// 与 Metal 的**刻意不同**：
//   ① **没有"驻留集 / 按需驻留"那一套**。上游那套是为 iOS 的 `MTLResidencySet` 而生（把整池读进内存、
//      钉成 wired、不计入 footprint，512 MB 渲染目标池一次没用过也占 530 MB）。Vulkan 没有这个语义，
//      内存一分配就在。所以这里整块省掉——**不是漏了，是不需要**。
//   ② **零拷贝不靠"套壳"**，靠的是**客体堆本身就是 Vulkan 内存**（`dm/ohos/vk_ctx_ohos.c`）。
//      所以池登记 = 在这块内存上建一个 `VkBuffer`、绑到池的偏移上；全程没有任何拷贝。
// Two deliberate differences from Metal: no residency set / on-demand residency (an iOS concept), and
// zero-copy comes from the guest heap itself being Vulkan memory rather than from wrapping host memory.
//
// **尚未做（后续小步）**：贴图要 `VkImage` 而不是 `VkBuffer`，且 Vulkan 不允许缓冲与图像**同时**
// 占用同一段内存（有 `bufferImageGranularity` 约束）——所以池里同时住着缓冲和贴图这件事，要在贴图那一步
// 单独设计（分槽 / 分区登记）。本步只覆盖"缓冲"。
// Not yet done (later steps): textures need VkImage, and Vulkan forbids a buffer and an image overlapping
// the same memory range, so pools holding both need a dedicated design in the texture step.
#pragma once

#include <stdint.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// 池登记 / 注销 —— 对标 `mtl_pool_add` / `mtl_pool_del`。
// 返回 0 = 成功；非 0 = 失败（池不在客体堆里、建缓冲失败……），原因见 nvn_vk_last_error()。
int  nvn_vk_pool_add(const void *mem, uint64_t size);
void nvn_vk_pool_del(const void *mem);

// 地址反查 —— 对标 `mtl_addr` / `mtl_gpu_addr`。
// `addr..addr+len` 整个落在某个池里时：`*buf` = 该池的 VkBuffer，`*off` = `addr − 池基址`
// （就是绑定点/动态偏移要用的那个数），`*gpu` = 该地址的设备地址。
// 返回 1 = 命中，0 = 没有池覆盖这个区间（调用方据此报错）。
int  nvn_vk_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off);
int  nvn_vk_gpu_addr(uint64_t addr, uint64_t len, VkBuffer *buf, uint64_t *off, uint64_t *gpu);

// 诊断
int          nvn_vk_pool_count(void);
const char  *nvn_vk_last_error(void);

// ---------------------------------------------------------------- 回放（对标 mtl_exec）

// **着色器解析接缝**：回放层不自己去读表，而是问这个回调"控制段哈希 + 阶段 → SPIR-V 在哪"。
// 为什么留这道缝：上游 Metal 侧拿的是离线编好的 metallib（`shader_map.tsv` + `dmsh_build.py`），
// 而 Vulkan 侧要的是 **SPIR-V**——那张表要靠游戏文件才能生成（L3/离线），现在还不存在。
// 留缝之后，"回放骨架"能立刻验，"着色器表"以后补，两边互不阻塞。
// 返回 1 = 找到（out 填好），0 = 没有。
typedef struct { const uint32_t *spirv; uint32_t nwords; } nvn_vk_shader;
typedef int (*nvn_vk_shader_fn)(const void *control, int stage, nvn_vk_shader *out);
void nvn_vk_set_shader_resolver(nvn_vk_shader_fn fn);

// 一次"回放提交"：begin 拿一个命令缓冲开始录制，exec 往里录，end 提交并等完成。
// 对标上游：mtl_exec 录进"当前提交"，提交点由外层管。0 = 成功。
int  nvn_vk_frame_begin(void);
int  nvn_vk_frame_end(void);

// 把一段命令流（handle = 段首地址，格式见 nvn_obj.h）回放成 Vulkan 命令。对标 mtl_exec。
void nvn_vk_exec(const uint8_t *handle);

// 统计（判据用）：绘制次数 / 新建管线数 / 命中缓存数 / 走过的命令条数 / 未处理的命令条数。
//
// **测量骨架**（对标上游那套 `DM_NVN_*` 诊断）：为什么先做它——"以性能优化为标准"的前提是
// **能说出这一步花了多少 GPU 时间、搬了多少字节**；没有这个，优化就只能靠感觉。
// 上游有逐通道 GPU 计时、绘制/通道数、附件读入写出字节数、贴图上传耗时……这里先落前四项。
typedef struct {
    uint64_t ops, ops_skipped;      // 走过的命令条数 / 未处理的条数（不静默）
    uint64_t draws, passes;         // 绘制次数 / 渲染通道数
    uint64_t pso_built, pso_hit;    // 新建管线数 / 命中缓存数
    uint64_t pso_build_ns;          // 建管线累计耗时（着色器模块 + 管线创建）——"落盘缓存"的判据
    uint64_t pso_cache_blob;        // 启动时从盘上载入的管线缓存字节数（0 = 冷启动）
    uint64_t bw_load, bw_store;     // 附件读入 / 写出字节（按通道尺寸 × 像素字节估算）
    uint64_t gpu_ns;                // 本帧 GPU 耗时（时间戳差值；0 = 设备不支持或没采到）
    uint64_t pass_gpu_ns[8];        // 逐通道 GPU 耗时（前 8 个通道）
    uint64_t pass_n;                // 本帧通道数
} nvn_vk_stats_t;
void nvn_vk_stats_get(nvn_vk_stats_t *out);
void nvn_vk_stats_reset(void);

// ---- 管线缓存落盘：**Vulkan 特有、Metal 做不到**的一项 ----
// 建管线 = 驱动编译着色器，是首帧卡顿的主要来源。把 `VkPipelineCache` 存到盘上，第二次启动
// 直接命中 ⇒ 零编译。上游 Metal 侧只有"离线 metallib + 预热清单"（构建期产物），
// **做不到运行时积累**——这是鸿蒙侧可以比上游做得好的地方。
//
// path = 缓存文件路径（空串 = 关闭落盘）。文件不存在时按冷启动建缓存；存在则载入。
// 每次提交后若有新管线，自动存盘。返回 0 = 成功。
int  nvn_vk_pso_cache_enable(const char *path);
// 诊断用：清空**内存里**的管线表（下次绘制会重新建管线，用来对照"冷 / 热"两次建管线的耗时）。
void nvn_vk_pso_local_clear(void);

// 当前渲染目标的 Vulkan 图像（读回 / 将来的出图用）。没设过渲染目标时返回 VK_NULL_HANDLE。
// 通道结束时它处于 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL。
VkImage       nvn_vk_target_image(void);
int           nvn_vk_target_extent(uint32_t *w, uint32_t *h);
VkImageLayout nvn_vk_target_layout(void);

// ---------------------------------------------------------------- 出图合流：外部渲染目标
// 回放层平时**自建**渲染目标（`finalLayout = TRANSFER_SRC`，供读回）。但"画到屏幕"要求
// 画进**交换链图像**、且通道结束布局是 `PRESENT_SRC_KHR` —— 所以留这道缝：
//   把外部目标登记在某个贴图对象上，回放层按命令流选目标时遇到它就直接用调用方的
//   图像 / 通道 / 帧缓冲，**不再自建**；没登记时行为与原来一字不差。
// 所有权：图像、通道、帧缓冲**都由调用方持有与销毁**，回放层只是引用（内部不打销毁标记）。
// 典型用法（见 vk_backend_onscreen_replay）：每帧取到交换链图像后重新绑一次即可。
int  nvn_vk_target_bind_external(const void *tex, VkImage img, VkImageView view, VkRenderPass rp,
                                 VkFramebuffer fb, uint32_t w, uint32_t h, VkFormat fmt,
                                 VkImageLayout final_layout);
void nvn_vk_target_unbind_external(const void *tex);

#ifdef __cplusplus
}
#endif
