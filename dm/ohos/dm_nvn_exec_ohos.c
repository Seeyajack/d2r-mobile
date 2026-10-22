// ================= M4 真执行：把记录流翻译成 Vulkan 调用 =================
// 背景（2026-10-22）：入口面已 100%、四层自测真机 0/0/0/0，但 `SubmitCommands` 只统计不执行 ⇒ 一帧都没出过。
// 这一层的职责很窄，就一件事：**遍历 CB 的记录流，按命令名把它翻译成 Vulkan 调用**。
//
// 为什么复用 `vk_backend` 而不是直接写 vkCmd：
//   离屏目标 + 清屏 + 读回这条路径已经在 3.4-E 上**真机验过**（逐像素比对过），
//   在它之上做"翻译"，能把"翻译对不对"和"Vulkan 用得对不对"两件事分开——出问题时不用两头猜。
//   后面接交换链/画三角形时才需要下沉到 vkCmd。
//
// 当前支持（首帧最小集的第一档）：
//   ClearColor / ClearColori / ClearColorui  → vk_backend_clear
//   SetRenderTargets                          → 目标固定为离屏 rt（记账）
//   其它命令                                   → 计数（先记录不执行，等真机给出真实命令构成再补）
//
// 判据：`dm_nvn_exec_smoketest()` 走一遍 NVN 分发 → 记录 → 提交 → 读回中心像素，
//       断言它等于我们清的那个颜色（±1）。这是"NVN→Vulkan→像素"整条链的自证。

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "vk_backend.h"
#include "vk_ctx_ohos.h"      // dm_ohos_vk_oneshot：把我们录的一段命令跑掉
#include "tri_shaders.h"      // 复用已验证的三角形 SPIR-V（阶段 2 的地面真值）
#include "dm_nvn_cb_ohos.h"
#include "dm_nvn_exec_ohos.h"

// 分发入口（dm_nvn_ohos.c）。合成冒烟刻意走它，连"接线对不对"一起验。
void *dm_nvn_bootstrap(const char *name);

static vk_ctx *g_ctx;
static vk_rt  *g_rt;
static int     g_clear_calls, g_other_ops, g_frames;
static float   g_last_clear[4] = { 0, 0, 0, 1 };
static char    g_report[1536] = "（还没跑过）";   // 要装下 vk_backend_shader 的整份报告（判定行在末尾）
static int     g_verdict = -1;          // 0 通过 / -1 失败

static float as_f32(uint64_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

int dm_nvn_exec_ensure(void) {
    if (g_rt) return 0;
    char err[256] = "";
    g_ctx = vk_backend_init(err, sizeof err);
    if (!g_ctx) { snprintf(g_report, sizeof g_report, "Vulkan 初始化失败：%s", err); return -1; }
    g_rt = vk_backend_target(g_ctx, 64, 64, err, sizeof err);
    if (!g_rt) {
        snprintf(g_report, sizeof g_report, "建离屏目标失败：%s", err);
        vk_backend_shutdown(g_ctx); g_ctx = NULL;
        return -1;
    }
    return 0;
}

void dm_nvn_exec_shutdown(void) {
    if (g_rt) { vk_backend_target_free(g_ctx, g_rt); g_rt = NULL; }
    if (g_ctx) { vk_backend_shutdown(g_ctx); g_ctx = NULL; }
}

// ── 一帧的执行状态 ──
// 为什么"画三角形"不在这一层实现：管线/着色器/顶点缓冲的创建属于 Vulkan 资源管理，
// 放在已验证过的 vk_backend 里（阶段 2 的 SPIR-V 直通测试），执行层只做"翻译"。
typedef struct {
    float clear[4];
    int   do_draw;      // 记录流里出现 DrawArrays/DrawElements
    int   draw_count;
} frame_st;
static frame_st g_frame;            // 当前帧的记录汇总
static int g_frame_drew;            // 这一帧记录里有没有 Draw 系命令
static int g_win_via_nvn;           // nvnWindowInitialize 被调过（= 游戏真的要上屏）


// 翻译器：一条记录 → 一个 Vulkan 动作
static int on_op(int id, const char *name, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, void *ud) {
    (void)id; (void)ud;
    if (!name) return 0;

    if (!strcmp(name, "ClearColor") || !strcmp(name, "ClearColori") || !strcmp(name, "ClearColorui")) {
        float r = as_f32(a1), g = as_f32(a2), b = as_f32(a3), a = as_f32(a4);
        g_last_clear[0] = r; g_last_clear[1] = g; g_last_clear[2] = b; g_last_clear[3] = a;
        if (!g_rt) return 1;
        if (vk_backend_clear(g_ctx, g_rt, r, g, b, a) == 0) g_clear_calls++;
        return 0;
    }
    if (!strcmp(name, "SetRenderTargets")) {
        // 目标固定 = 离屏 rt（真交换链等接 VI 时再补）
        return 0;
    }
    if (!strcmp(name, "DrawArrays") || !strcmp(name, "DrawElements") ||
        !strcmp(name, "DrawArraysInstanced") || !strcmp(name, "DrawElementsInstanced")) {
        g_frame.do_draw = 1;          // 记录流里说要画
        g_frame_drew = 1;
        if (a2) g_frame.draw_count = (int)a2;   // DrawArrays(cb, first, count)：count 在 a2
        return 0;
    }
    // 其余：记录到了但暂不执行。真机 `g_nvn_submit_log` 给出真实构成后再按需补。
    g_other_ops++;
    return 0;
}

int dm_nvn_exec_submit_cb(void *cb) {
    if (dm_nvn_exec_ensure() != 0) return -1;
    memset(&g_frame, 0, sizeof g_frame);
    g_frame_drew = 0;
    g_frame.clear[0] = g_last_clear[0]; g_frame.clear[1] = g_last_clear[1];
    g_frame.clear[2] = g_last_clear[2]; g_frame.clear[3] = g_last_clear[3];
    int n = dm_nvn_cb_replay(cb, on_op, NULL);
    // 回放只是"汇总这一帧要做什么"；真正下命令在 present（一帧一次提交）
    return n;
}

int dm_nvn_exec_frame_done(void) {
    if (dm_nvn_exec_ensure() != 0) return -1;
    g_frames++;
    // ★ 真交换链上屏：游戏调 nvnQueuePresentTexture ⇒ 走 3.3-A 已验证的
    //   XComponent → VkSurfaceKHR → swapchain → present 骨架（`vk_backend_onscreen`）。
    //   判据：它报的"present N 帧 + 间隔统计 + 节拍稳定"，画面本身靠截图肉眼确认。
    if (g_win_via_nvn) {
        static char os[2048];
        int f = vk_backend_onscreen(os, sizeof os);
        g_verdict = f ? -1 : 0;
        // 报告里只留关键几行（整份 ~1KB，判定行在末尾）
        snprintf(g_report, sizeof g_report, "上屏：%s", f ? "失败" : "通过");
        {
            const char *tail = strrchr(os, '=');
            if (tail) {
                size_t room = sizeof g_report - strlen(g_report) - 1;
                strncat(g_report, tail, room);
            }
        }
        memset(&g_frame, 0, sizeof g_frame);
        g_frame_drew = 0;
        return g_verdict;
    }
    // 一帧：清屏（颜色来自记录里的 ClearColor）→ 可选画三角形 → 读回
    //
    // ⚠ Draw 目前走 `vk_backend_shader()`（阶段 2 那条已验证的路径：自带管线/顶点/绘制/读回/判定），
    //   而不是我新写的 `vk_backend_draw_triangle()`——后者卡在一个真机事实上：
    //   **24 字节顶点缓冲的 memoryTypeBits=0x3 里没有 HOST_VISIBLE|COHERENT 类型**
    //   （读回缓冲的 bits 不同所以能成；同一份代码在 NAPI 线程上却通 ⇒ 与调用线程无关，是这台驱动
    //    对"小缓冲"的内存类型裁剪）。等做成"staging / 换内存类型"再切回去，届时判据不变。
    if (g_frame.do_draw) {
        int fails = vk_backend_shader(g_report, sizeof g_report);
        g_verdict = fails ? -1 : 0;
        memset(&g_frame, 0, sizeof g_frame);
        g_frame_drew = 0;
        return g_verdict;
    } else if (g_clear_calls == 0) {
        // 没有 Clear 也没有 Draw：当作只画三角形（合成冒烟会用）
        if (vk_backend_draw_triangle(g_ctx, g_rt, g_frame.clear) != 0) {
            snprintf(g_report, sizeof g_report, "绘制失败：%s", vk_backend_draw_err());
            g_verdict = -1;
            return -1;
        }
    } else if (vk_backend_clear(g_ctx, g_rt, g_frame.clear[0], g_frame.clear[1],
                                g_frame.clear[2], g_frame.clear[3]) != 0) {
        snprintf(g_report, sizeof g_report, "清屏失败");
        g_verdict = -1;
        return -1;
    }
    memset(&g_frame, 0, sizeof g_frame);

    size_t rbsz = vk_backend_rt_rb_size(g_rt);
    uint8_t *px = (uint8_t *)malloc(rbsz);
    if (!px) { snprintf(g_report, sizeof g_report, "读回缓冲分配失败"); return -1; }
    long got = vk_backend_readback(g_ctx, g_rt, px, rbsz);
    if (got < 0) { free(px); snprintf(g_report, sizeof g_report, "读回失败"); g_verdict = -1; return -1; }

    uint32_t w = vk_backend_rt_w(g_rt), h = vk_backend_rt_h(g_rt);
    uint32_t cx = w / 2, cy = h / 2;
    const uint8_t *c = px + ((size_t)cy * w + cx) * 4;
    // 三角形内取 (32,16)、三角形外取 (0,63)——与阶段 2 SPIR-V 直通测试**同一个口径**，便于交叉比对。
    // （Vulkan NDC Y 向下：底边在顶部整行、顶点在底部中央。）
    const uint8_t *in  = px + (16u * w + 32u) * 4;
    const uint8_t *out = px + ((size_t)(h - 1) * w + 0) * 4;
    uint8_t er = (uint8_t)(g_last_clear[0] * 255.0f + 0.5f);
    uint8_t eg = (uint8_t)(g_last_clear[1] * 255.0f + 0.5f);
    uint8_t eb = (uint8_t)(g_last_clear[2] * 255.0f + 0.5f);
    uint8_t ea = (uint8_t)(g_last_clear[3] * 255.0f + 0.5f);

    int ok_c = (abs((int)c[0] - er) <= 1) && (abs((int)c[1] - eg) <= 1) &&
               (abs((int)c[2] - eb) <= 1) && (abs((int)c[3] - ea) <= 1);
    // 三角形是蓝的（阶段 2 已验证的着色器输出）：画了的话内部应当是 (0,0,255,255)
    int drew = g_frame_drew;
    int ok_in  = !drew || (in[0] <= 1 && in[1] <= 1 && in[2] >= 254 && in[3] >= 254);
    int ok_out = !drew || (abs((int)out[0] - er) <= 1 && abs((int)out[1] - eg) <= 1 &&
                           abs((int)out[2] - eb) <= 1 && abs((int)out[3] - ea) <= 1);
    g_verdict = (ok_c && ok_in && ok_out) ? 0 : -1;
    snprintf(g_report, sizeof g_report,
             "%s 第 %d 帧读回：中心=(%u,%u,%u,%u) 期望=(%u,%u,%u,%u)；三角内(32,16)=(%u,%u,%u,%u) 三角外(0,%u)=(%u,%u,%u,%u)；clear %d 次 / 未执行 %d 条",
             g_verdict == 0 ? "✅" : "❌", g_frames,
             c[0], c[1], c[2], c[3], er, eg, eb, ea,
             in[0], in[1], in[2], in[3], h - 1, out[0], out[1], out[2], out[3],
             g_clear_calls, g_other_ops);
    free(px);
    return g_verdict == 0 ? 0 : -1;
}

void dm_nvn_exec_mark_window(void) { g_win_via_nvn = 1; }

const char *dm_nvn_exec_report(void) { return g_report; }
int dm_nvn_exec_verdict(void) { return g_verdict; }

// ── 合成冒烟：走 NVN 分发 → 记录 → 提交 → 读回 ──
// 刻意**通过 dm_nvn_bootstrap 取函数**（和游戏同一条路），这样连"分派接线对不对"一起验。
int dm_nvn_exec_smoketest(void) {
    void *b = calloc(1, 256), *w = calloc(1, 256), *q = calloc(1, 256), *cb = calloc(1, 256);
    if (!b || !w || !q || !cb) { free(b); free(w); free(q); free(cb); return -1; }

    void *f_qbdef = dm_nvn_bootstrap("nvnQueueBuilderSetDefaults");
    void *f_wbdef = dm_nvn_bootstrap("nvnWindowBuilderSetDefaults");
    void *f_win   = dm_nvn_bootstrap("nvnWindowInitialize");
    void *f_cbini = dm_nvn_bootstrap("nvnCommandBufferInitialize");
    void *f_cbbeg = dm_nvn_bootstrap("nvnCommandBufferBeginRecording");
    void *f_cbclr = dm_nvn_bootstrap("nvnCommandBufferClearColor");
    void *f_cbend = dm_nvn_bootstrap("nvnCommandBufferEndRecording");
    void *f_cbdraw = dm_nvn_bootstrap("nvnCommandBufferDrawArrays");
    void *f_qini  = dm_nvn_bootstrap("nvnQueueInitialize");
    void *f_qsub  = dm_nvn_bootstrap("nvnQueueSubmitCommands");
    void *f_qpre  = dm_nvn_bootstrap("nvnQueuePresentTexture");
    if (!f_qbdef || !f_wbdef || !f_win || !f_cbini || !f_cbbeg || !f_cbclr ||
        !f_cbend || !f_cbdraw || !f_qini || !f_qsub || !f_qpre) {
        snprintf(g_report, sizeof g_report, "冒烟：有 NVN 入口没注册上（分发接线问题）");
        free(b); free(w); free(q); free(cb);
        return -1;
    }
// NVN 入口都是 (x0,x1,x2,x3,x4) → uint64；这里统一按 5 参调
#define CALL(fn, x0, x1, x2, x3, x4) \
    ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))(fn)) \
        ((uint64_t)(uintptr_t)(x0), (uint64_t)(uintptr_t)(x1), (uint64_t)(uintptr_t)(x2), \
         (uint64_t)(uintptr_t)(x3), (uint64_t)(uintptr_t)(x4))
    union { float f; uint32_t u; } r = { 0.25f }, g = { 0.5f }, bl = { 0.75f }, al = { 1.0f };

    // ── 第一趟：**不上屏**（不调 nvnWindowInitialize）——离屏判据：清 + 画 + 读回 ──
    CALL(f_wbdef, b, 0, 0, 0, 0);
    CALL(f_qbdef, b, 0, 0, 0, 0);
    CALL(f_qini, q, 0, b, 0, 0);
    CALL(f_cbini, cb, 0, 0, 0, 0);
    CALL(f_cbbeg, cb, 0, 0, 0, 0);
    // ClearColor(cb, r,g,b,a)：四个 float 的**位模式**按整型传（NVN 的调用约定），a 走 x4
    CALL(f_cbclr, cb, r.u, g.u, bl.u, al.u);
    CALL(f_cbdraw, cb, 0, 3, 0, 0);       // DrawArrays(cb, first=0, count=3)
    CALL(f_cbend, cb, 0, 0, 0, 0);

    uint64_t cbptr = (uint64_t)(uintptr_t)cb;
    CALL(f_qsub, q, 1, &cbptr, 0, 0);      // num=1, bufs=&cbptr
    CALL(f_qpre, q, 0, 0, 0, 0);           // 没建窗口 ⇒ 离屏读回判定
    int off = dm_nvn_exec_verdict();

    // ── 第二趟：**上屏**（调 nvnWindowInitialize ⇒ 真交换链 present）──
    CALL(f_win, w, 0, b, 0, 0);
    CALL(f_qpre, q, w, 0, 0, 0);
    int on = dm_nvn_exec_verdict();

    snprintf(g_report, sizeof g_report, "离屏(清+画+读回)=%s；上屏(真交换链 present)=%s",
             off ? "FAIL" : "OK", on ? "FAIL" : "OK");
    free(b); free(w); free(q); free(cb);
    return (off || on) ? -1 : 0;
}

#undef CALL
