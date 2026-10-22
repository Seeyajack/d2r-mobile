// ================= M4-c 第二步：NVN 命令缓冲记录层 =================
// 由 dm/tools/gen_cb_layer.py 生成（名字表来自游戏 .rodata 的实际需求，127 个）。
// 改法：改生成器后重跑，不要手改本文件。
#include <string.h>
#include <stdint.h>
#include "dm_nvn_cb_ohos.h"

#define CB_MAX 16
#define CB_OPS 8192
#define CB_ARGS 4

typedef struct { uint16_t id; uint32_t n; uint64_t a[CB_ARGS]; } cb_op;

typedef struct {
    const void *p;
    int used;
    int recording;
    uint32_t nops;
    uint64_t cmd_mem, ctl_mem;      // 通过 Add*Memory 报上来的容量
    uint64_t callback, callback_data;
    uint32_t per_id[127];           // 每条命令各记了几次（摘要用）
    cb_op ops[CB_OPS];
} cb_obj;

static cb_obj *g_cb[CB_MAX];
static int g_ncb;

static cb_obj *cb_get(const void *p, int create) {
    if (!p) return NULL;
    for (int i = 0; i < g_ncb; i++) if (g_cb[i]->p == p) return g_cb[i];
    if (!create || g_ncb >= CB_MAX) return NULL;
    cb_obj *o = (cb_obj *)calloc(1, sizeof *o);
    if (!o) return NULL;
    o->p = p; o->used = 1;
    g_cb[g_ncb++] = o;
    return o;
}

static void cb_push(cb_obj *o, int id, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!o) return;
    o->per_id[id]++;
    if (o->nops < CB_OPS) {
        cb_op *e = &o->ops[o->nops++];
        e->id = (uint16_t)id; e->n = 0;
        e->a[0] = a0; e->a[1] = a1; e->a[2] = a2; e->a[3] = a3;
    }
}

static const char *const g_cb_names[127] = {
    "AddCommandMemory",
    "AddControlMemory",
    "Barrier",
    "BeginRecording",
    "BeginTransformFeedback",
    "BindBlendState",
    "BindChannelMaskState",
    "BindColorState",
    "BindCoverageModulationTable",
    "BindDepthStencilState",
    "BindImage",
    "BindImages",
    "BindMultisampleState",
    "BindPolygonState",
    "BindProgram",
    "BindStorageBuffer",
    "BindStorageBuffers",
    "BindTexture",
    "BindTextures",
    "BindTransformFeedbackBuffer",
    "BindTransformFeedbackBuffers",
    "BindUniformBuffer",
    "BindUniformBuffers",
    "BindVertexAttribState",
    "BindVertexBuffer",
    "BindVertexBuffers",
    "BindVertexStreamState",
    "CallCommands",
    "ClearBuffer",
    "ClearColor",
    "ClearColori",
    "ClearColorui",
    "ClearDepthStencil",
    "ClearTexture",
    "ClearTexturei",
    "ClearTextureui",
    "CopyBufferToBuffer",
    "CopyBufferToTexture",
    "CopyCommands",
    "CopyTextureToBuffer",
    "CopyTextureToTexture",
    "DiscardColor",
    "DiscardDepthStencil",
    "DispatchCompute",
    "DispatchComputeIndirect",
    "Downsample",
    "DrawArrays",
    "DrawArraysIndirect",
    "DrawArraysInstanced",
    "DrawElements",
    "DrawElementsBaseVertex",
    "DrawElementsIndirect",
    "DrawElementsInstanced",
    "DrawTexture",
    "DrawTransformFeedback",
    "EndRecording",
    "EndTransformFeedback",
    "FenceSync",
    "Finalize",
    "GetCommandMemoryFree",
    "GetCommandMemorySize",
    "GetCommandMemoryUsed",
    "GetControlMemoryFree",
    "GetControlMemorySize",
    "GetControlMemoryUsed",
    "GetCopyImageStride",
    "GetCopyRowStride",
    "GetMemoryCallback",
    "GetMemoryCallbackData",
    "Initialize",
    "InsertDebugMarker",
    "IsRecording",
    "MultiDrawArraysIndirectCount",
    "MultiDrawElementsIndirectCount",
    "PauseTransformFeedback",
    "PopDebugGroup",
    "PushDebugGroup",
    "ReportCounter",
    "ReportValue",
    "ResetCounter",
    "ResolveDepthBuffer",
    "RestoreZCullData",
    "ResumeTransformFeedback",
    "SaveZCullData",
    "SetAlphaRef",
    "SetBlendColor",
    "SetConservativeRasterDilate",
    "SetConservativeRasterEnable",
    "SetCopyImageStride",
    "SetCopyRowStride",
    "SetDebugLabel",
    "SetDepthBounds",
    "SetDepthClamp",
    "SetDepthRange",
    "SetDepthRanges",
    "SetInnerTessellationLevels",
    "SetLineWidth",
    "SetMemoryCallback",
    "SetMemoryCallbackData",
    "SetOuterTessellationLevels",
    "SetPatchSize",
    "SetPointSize",
    "SetPolygonOffsetClamp",
    "SetPrimitiveRestart",
    "SetProgramSubroutines",
    "SetRasterizerDiscard",
    "SetRenderEnable",
    "SetRenderEnableConditional",
    "SetRenderTargets",
    "SetSampleMask",
    "SetSamplerPool",
    "SetScissor",
    "SetScissors",
    "SetShaderScratchMemory",
    "SetStencilMask",
    "SetStencilRef",
    "SetStencilValueMask",
    "SetSubpixelPrecisionBias",
    "SetTexturePool",
    "SetTiledCacheAction",
    "SetTiledCacheTileSize",
    "SetViewport",
    "SetViewportSwizzles",
    "SetViewports",
    "TiledDownsample",
    "UpdateUniformBuffer",
    "WaitSync",
};

static uint64_t cb_2(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 2, a1, a2, a3, 0); return 0;
}
static uint64_t cb_4(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 4, a1, a2, a3, 0); return 0;
}
static uint64_t cb_5(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 5, a1, a2, a3, 0); return 0;
}
static uint64_t cb_6(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 6, a1, a2, a3, 0); return 0;
}
static uint64_t cb_7(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 7, a1, a2, a3, 0); return 0;
}
static uint64_t cb_8(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 8, a1, a2, a3, 0); return 0;
}
static uint64_t cb_9(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 9, a1, a2, a3, 0); return 0;
}
static uint64_t cb_10(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 10, a1, a2, a3, 0); return 0;
}
static uint64_t cb_11(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 11, a1, a2, a3, 0); return 0;
}
static uint64_t cb_12(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 12, a1, a2, a3, 0); return 0;
}
static uint64_t cb_13(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 13, a1, a2, a3, 0); return 0;
}
static uint64_t cb_14(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 14, a1, a2, a3, 0); return 0;
}
static uint64_t cb_15(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 15, a1, a2, a3, 0); return 0;
}
static uint64_t cb_16(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 16, a1, a2, a3, 0); return 0;
}
static uint64_t cb_17(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 17, a1, a2, a3, 0); return 0;
}
static uint64_t cb_18(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 18, a1, a2, a3, 0); return 0;
}
static uint64_t cb_19(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 19, a1, a2, a3, 0); return 0;
}
static uint64_t cb_20(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 20, a1, a2, a3, 0); return 0;
}
static uint64_t cb_21(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 21, a1, a2, a3, 0); return 0;
}
static uint64_t cb_22(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 22, a1, a2, a3, 0); return 0;
}
static uint64_t cb_23(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 23, a1, a2, a3, 0); return 0;
}
static uint64_t cb_24(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 24, a1, a2, a3, 0); return 0;
}
static uint64_t cb_25(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 25, a1, a2, a3, 0); return 0;
}
static uint64_t cb_26(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 26, a1, a2, a3, 0); return 0;
}
static uint64_t cb_27(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 27, a1, a2, a3, 0); return 0;
}
static uint64_t cb_28(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 28, a1, a2, a3, 0); return 0;
}
static uint64_t cb_29(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 29, a1, a2, a3, 0); return 0;
}
static uint64_t cb_30(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 30, a1, a2, a3, 0); return 0;
}
static uint64_t cb_31(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 31, a1, a2, a3, 0); return 0;
}
static uint64_t cb_32(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 32, a1, a2, a3, 0); return 0;
}
static uint64_t cb_33(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 33, a1, a2, a3, 0); return 0;
}
static uint64_t cb_34(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 34, a1, a2, a3, 0); return 0;
}
static uint64_t cb_35(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 35, a1, a2, a3, 0); return 0;
}
static uint64_t cb_36(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 36, a1, a2, a3, 0); return 0;
}
static uint64_t cb_37(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 37, a1, a2, a3, 0); return 0;
}
static uint64_t cb_38(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 38, a1, a2, a3, 0); return 0;
}
static uint64_t cb_39(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 39, a1, a2, a3, 0); return 0;
}
static uint64_t cb_40(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 40, a1, a2, a3, 0); return 0;
}
static uint64_t cb_41(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 41, a1, a2, a3, 0); return 0;
}
static uint64_t cb_42(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 42, a1, a2, a3, 0); return 0;
}
static uint64_t cb_43(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 43, a1, a2, a3, 0); return 0;
}
static uint64_t cb_44(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 44, a1, a2, a3, 0); return 0;
}
static uint64_t cb_45(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 45, a1, a2, a3, 0); return 0;
}
static uint64_t cb_46(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 46, a1, a2, a3, 0); return 0;
}
static uint64_t cb_47(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 47, a1, a2, a3, 0); return 0;
}
static uint64_t cb_48(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 48, a1, a2, a3, 0); return 0;
}
static uint64_t cb_49(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 49, a1, a2, a3, 0); return 0;
}
static uint64_t cb_50(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 50, a1, a2, a3, 0); return 0;
}
static uint64_t cb_51(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 51, a1, a2, a3, 0); return 0;
}
static uint64_t cb_52(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 52, a1, a2, a3, 0); return 0;
}
static uint64_t cb_53(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 53, a1, a2, a3, 0); return 0;
}
static uint64_t cb_54(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 54, a1, a2, a3, 0); return 0;
}
static uint64_t cb_56(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 56, a1, a2, a3, 0); return 0;
}
static uint64_t cb_57(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 57, a1, a2, a3, 0); return 0;
}
static uint64_t cb_65(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 65, a1, a2, a3, 0); return 0;
}
static uint64_t cb_66(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 66, a1, a2, a3, 0); return 0;
}
static uint64_t cb_70(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 70, a1, a2, a3, 0); return 0;
}
static uint64_t cb_72(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 72, a1, a2, a3, 0); return 0;
}
static uint64_t cb_73(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 73, a1, a2, a3, 0); return 0;
}
static uint64_t cb_74(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 74, a1, a2, a3, 0); return 0;
}
static uint64_t cb_75(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 75, a1, a2, a3, 0); return 0;
}
static uint64_t cb_76(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 76, a1, a2, a3, 0); return 0;
}
static uint64_t cb_77(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 77, a1, a2, a3, 0); return 0;
}
static uint64_t cb_78(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 78, a1, a2, a3, 0); return 0;
}
static uint64_t cb_79(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 79, a1, a2, a3, 0); return 0;
}
static uint64_t cb_80(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 80, a1, a2, a3, 0); return 0;
}
static uint64_t cb_81(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 81, a1, a2, a3, 0); return 0;
}
static uint64_t cb_82(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 82, a1, a2, a3, 0); return 0;
}
static uint64_t cb_83(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 83, a1, a2, a3, 0); return 0;
}
static uint64_t cb_84(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 84, a1, a2, a3, 0); return 0;
}
static uint64_t cb_85(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 85, a1, a2, a3, 0); return 0;
}
static uint64_t cb_86(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 86, a1, a2, a3, 0); return 0;
}
static uint64_t cb_87(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 87, a1, a2, a3, 0); return 0;
}
static uint64_t cb_88(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 88, a1, a2, a3, 0); return 0;
}
static uint64_t cb_89(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 89, a1, a2, a3, 0); return 0;
}
static uint64_t cb_90(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 90, a1, a2, a3, 0); return 0;
}
static uint64_t cb_91(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 91, a1, a2, a3, 0); return 0;
}
static uint64_t cb_92(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 92, a1, a2, a3, 0); return 0;
}
static uint64_t cb_93(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 93, a1, a2, a3, 0); return 0;
}
static uint64_t cb_94(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 94, a1, a2, a3, 0); return 0;
}
static uint64_t cb_95(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 95, a1, a2, a3, 0); return 0;
}
static uint64_t cb_96(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 96, a1, a2, a3, 0); return 0;
}
static uint64_t cb_99(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 99, a1, a2, a3, 0); return 0;
}
static uint64_t cb_100(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 100, a1, a2, a3, 0); return 0;
}
static uint64_t cb_101(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 101, a1, a2, a3, 0); return 0;
}
static uint64_t cb_102(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 102, a1, a2, a3, 0); return 0;
}
static uint64_t cb_103(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 103, a1, a2, a3, 0); return 0;
}
static uint64_t cb_104(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 104, a1, a2, a3, 0); return 0;
}
static uint64_t cb_105(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 105, a1, a2, a3, 0); return 0;
}
static uint64_t cb_106(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 106, a1, a2, a3, 0); return 0;
}
static uint64_t cb_107(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 107, a1, a2, a3, 0); return 0;
}
static uint64_t cb_108(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 108, a1, a2, a3, 0); return 0;
}
static uint64_t cb_109(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 109, a1, a2, a3, 0); return 0;
}
static uint64_t cb_110(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 110, a1, a2, a3, 0); return 0;
}
static uint64_t cb_111(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 111, a1, a2, a3, 0); return 0;
}
static uint64_t cb_112(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 112, a1, a2, a3, 0); return 0;
}
static uint64_t cb_113(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 113, a1, a2, a3, 0); return 0;
}
static uint64_t cb_114(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 114, a1, a2, a3, 0); return 0;
}
static uint64_t cb_115(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 115, a1, a2, a3, 0); return 0;
}
static uint64_t cb_116(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 116, a1, a2, a3, 0); return 0;
}
static uint64_t cb_117(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 117, a1, a2, a3, 0); return 0;
}
static uint64_t cb_118(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 118, a1, a2, a3, 0); return 0;
}
static uint64_t cb_119(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 119, a1, a2, a3, 0); return 0;
}
static uint64_t cb_120(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 120, a1, a2, a3, 0); return 0;
}
static uint64_t cb_121(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 121, a1, a2, a3, 0); return 0;
}
static uint64_t cb_122(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 122, a1, a2, a3, 0); return 0;
}
static uint64_t cb_123(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 123, a1, a2, a3, 0); return 0;
}
static uint64_t cb_124(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 124, a1, a2, a3, 0); return 0;
}
static uint64_t cb_125(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 125, a1, a2, a3, 0); return 0;
}
static uint64_t cb_126(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4; cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1); cb_push(o, 126, a1, a2, a3, 0); return 0;
}

// ── 生命周期 ──
static uint64_t cb_Initialize(uint64_t cb, uint64_t dev, uint64_t mem, uint64_t a3, uint64_t a4) {
    (void)dev; (void)mem; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1);
    if (o) { o->recording = 0; o->nops = 0; }
    return 0;
}
static uint64_t cb_Finalize(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    if (o) { o->recording = 0; o->nops = 0; }
    return 0;
}
static uint64_t cb_BeginRecording(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1);
    if (o) { o->recording = 1; o->nops = 0; memset(o->per_id, 0, sizeof o->per_id); }
    return 0;
}
static uint64_t cb_EndRecording(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    if (o) o->recording = 0;
    return 0;
}
static uint64_t cb_IsRecording(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    return o ? (uint64_t)o->recording : 0;
}

// ── 内存记账（游戏据此分配；返回非 0 会让它以为分配失败）──
static uint64_t cb_AddCommandMemory(uint64_t cb, uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1);
    if (o) o->cmd_mem += size;
    return 0;
}
static uint64_t cb_AddControlMemory(uint64_t cb, uint64_t size, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1);
    if (o) o->ctl_mem += size;
    return 0;
}
static uint64_t cb_CmdSize(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    return o ? o->cmd_mem : 0;
}
static uint64_t cb_CmdUsed(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    // 我们按"每条命令 16 字节"估，够游戏做"还剩多少"的判断，不会误判为 0
    return o ? (uint64_t)o->nops * 16u : 0;
}
static uint64_t cb_CmdFree(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    uint64_t used = o ? (uint64_t)o->nops * 16u : 0;
    uint64_t cap  = o ? o->cmd_mem : 0;
    return cap > used ? cap - used : 0;
}
static uint64_t cb_CtlSize(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    return o ? o->ctl_mem : 0;
}
static uint64_t cb_CtlUsed(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    return o ? (uint64_t)o->nops * 8u : 0;
}
static uint64_t cb_CtlFree(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    uint64_t used = o ? (uint64_t)o->nops * 8u : 0;
    uint64_t cap  = o ? o->ctl_mem : 0;
    return cap > used ? cap - used : 0;
}
// ── 内存回调（游戏可能用它接管分配；我们只记不用）──
static uint64_t cb_SetMemCb(uint64_t cb, uint64_t fn, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1);
    if (o) o->callback = fn;
    return 0;
}
static uint64_t cb_SetMemCbData(uint64_t cb, uint64_t d, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 1);
    if (o) o->callback_data = d;
    return 0;
}
static uint64_t cb_GetMemCb(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    return o ? o->callback : 0;
}
static uint64_t cb_GetMemCbData(uint64_t cb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    cb_obj *o = cb_get((const void *)(uintptr_t)cb, 0);
    return o ? o->callback_data : 0;
}

typedef struct { const char *name; void *fn; } cb_ent;
#define E(n, f) { n, (void *)(f) }
static const cb_ent g_cb_tab[127] = {
    E("nvnCommandBufferAddCommandMemory", cb_AddCommandMemory),
    E("nvnCommandBufferAddControlMemory", cb_AddControlMemory),
    E("nvnCommandBufferBarrier", cb_2),
    E("nvnCommandBufferBeginRecording", cb_BeginRecording),
    E("nvnCommandBufferBeginTransformFeedback", cb_4),
    E("nvnCommandBufferBindBlendState", cb_5),
    E("nvnCommandBufferBindChannelMaskState", cb_6),
    E("nvnCommandBufferBindColorState", cb_7),
    E("nvnCommandBufferBindCoverageModulationTable", cb_8),
    E("nvnCommandBufferBindDepthStencilState", cb_9),
    E("nvnCommandBufferBindImage", cb_10),
    E("nvnCommandBufferBindImages", cb_11),
    E("nvnCommandBufferBindMultisampleState", cb_12),
    E("nvnCommandBufferBindPolygonState", cb_13),
    E("nvnCommandBufferBindProgram", cb_14),
    E("nvnCommandBufferBindStorageBuffer", cb_15),
    E("nvnCommandBufferBindStorageBuffers", cb_16),
    E("nvnCommandBufferBindTexture", cb_17),
    E("nvnCommandBufferBindTextures", cb_18),
    E("nvnCommandBufferBindTransformFeedbackBuffer", cb_19),
    E("nvnCommandBufferBindTransformFeedbackBuffers", cb_20),
    E("nvnCommandBufferBindUniformBuffer", cb_21),
    E("nvnCommandBufferBindUniformBuffers", cb_22),
    E("nvnCommandBufferBindVertexAttribState", cb_23),
    E("nvnCommandBufferBindVertexBuffer", cb_24),
    E("nvnCommandBufferBindVertexBuffers", cb_25),
    E("nvnCommandBufferBindVertexStreamState", cb_26),
    E("nvnCommandBufferCallCommands", cb_27),
    E("nvnCommandBufferClearBuffer", cb_28),
    E("nvnCommandBufferClearColor", cb_29),
    E("nvnCommandBufferClearColori", cb_30),
    E("nvnCommandBufferClearColorui", cb_31),
    E("nvnCommandBufferClearDepthStencil", cb_32),
    E("nvnCommandBufferClearTexture", cb_33),
    E("nvnCommandBufferClearTexturei", cb_34),
    E("nvnCommandBufferClearTextureui", cb_35),
    E("nvnCommandBufferCopyBufferToBuffer", cb_36),
    E("nvnCommandBufferCopyBufferToTexture", cb_37),
    E("nvnCommandBufferCopyCommands", cb_38),
    E("nvnCommandBufferCopyTextureToBuffer", cb_39),
    E("nvnCommandBufferCopyTextureToTexture", cb_40),
    E("nvnCommandBufferDiscardColor", cb_41),
    E("nvnCommandBufferDiscardDepthStencil", cb_42),
    E("nvnCommandBufferDispatchCompute", cb_43),
    E("nvnCommandBufferDispatchComputeIndirect", cb_44),
    E("nvnCommandBufferDownsample", cb_45),
    E("nvnCommandBufferDrawArrays", cb_46),
    E("nvnCommandBufferDrawArraysIndirect", cb_47),
    E("nvnCommandBufferDrawArraysInstanced", cb_48),
    E("nvnCommandBufferDrawElements", cb_49),
    E("nvnCommandBufferDrawElementsBaseVertex", cb_50),
    E("nvnCommandBufferDrawElementsIndirect", cb_51),
    E("nvnCommandBufferDrawElementsInstanced", cb_52),
    E("nvnCommandBufferDrawTexture", cb_53),
    E("nvnCommandBufferDrawTransformFeedback", cb_54),
    E("nvnCommandBufferEndRecording", cb_EndRecording),
    E("nvnCommandBufferEndTransformFeedback", cb_56),
    E("nvnCommandBufferFenceSync", cb_57),
    E("nvnCommandBufferFinalize", cb_Finalize),
    E("nvnCommandBufferGetCommandMemoryFree", cb_CmdFree),
    E("nvnCommandBufferGetCommandMemorySize", cb_CmdSize),
    E("nvnCommandBufferGetCommandMemoryUsed", cb_CmdUsed),
    E("nvnCommandBufferGetControlMemoryFree", cb_CtlFree),
    E("nvnCommandBufferGetControlMemorySize", cb_CtlSize),
    E("nvnCommandBufferGetControlMemoryUsed", cb_CtlUsed),
    E("nvnCommandBufferGetCopyImageStride", cb_65),
    E("nvnCommandBufferGetCopyRowStride", cb_66),
    E("nvnCommandBufferGetMemoryCallback", cb_GetMemCb),
    E("nvnCommandBufferGetMemoryCallbackData", cb_GetMemCbData),
    E("nvnCommandBufferInitialize", cb_Initialize),
    E("nvnCommandBufferInsertDebugMarker", cb_70),
    E("nvnCommandBufferIsRecording", cb_IsRecording),
    E("nvnCommandBufferMultiDrawArraysIndirectCount", cb_72),
    E("nvnCommandBufferMultiDrawElementsIndirectCount", cb_73),
    E("nvnCommandBufferPauseTransformFeedback", cb_74),
    E("nvnCommandBufferPopDebugGroup", cb_75),
    E("nvnCommandBufferPushDebugGroup", cb_76),
    E("nvnCommandBufferReportCounter", cb_77),
    E("nvnCommandBufferReportValue", cb_78),
    E("nvnCommandBufferResetCounter", cb_79),
    E("nvnCommandBufferResolveDepthBuffer", cb_80),
    E("nvnCommandBufferRestoreZCullData", cb_81),
    E("nvnCommandBufferResumeTransformFeedback", cb_82),
    E("nvnCommandBufferSaveZCullData", cb_83),
    E("nvnCommandBufferSetAlphaRef", cb_84),
    E("nvnCommandBufferSetBlendColor", cb_85),
    E("nvnCommandBufferSetConservativeRasterDilate", cb_86),
    E("nvnCommandBufferSetConservativeRasterEnable", cb_87),
    E("nvnCommandBufferSetCopyImageStride", cb_88),
    E("nvnCommandBufferSetCopyRowStride", cb_89),
    E("nvnCommandBufferSetDebugLabel", cb_90),
    E("nvnCommandBufferSetDepthBounds", cb_91),
    E("nvnCommandBufferSetDepthClamp", cb_92),
    E("nvnCommandBufferSetDepthRange", cb_93),
    E("nvnCommandBufferSetDepthRanges", cb_94),
    E("nvnCommandBufferSetInnerTessellationLevels", cb_95),
    E("nvnCommandBufferSetLineWidth", cb_96),
    E("nvnCommandBufferSetMemoryCallback", cb_SetMemCb),
    E("nvnCommandBufferSetMemoryCallbackData", cb_SetMemCbData),
    E("nvnCommandBufferSetOuterTessellationLevels", cb_99),
    E("nvnCommandBufferSetPatchSize", cb_100),
    E("nvnCommandBufferSetPointSize", cb_101),
    E("nvnCommandBufferSetPolygonOffsetClamp", cb_102),
    E("nvnCommandBufferSetPrimitiveRestart", cb_103),
    E("nvnCommandBufferSetProgramSubroutines", cb_104),
    E("nvnCommandBufferSetRasterizerDiscard", cb_105),
    E("nvnCommandBufferSetRenderEnable", cb_106),
    E("nvnCommandBufferSetRenderEnableConditional", cb_107),
    E("nvnCommandBufferSetRenderTargets", cb_108),
    E("nvnCommandBufferSetSampleMask", cb_109),
    E("nvnCommandBufferSetSamplerPool", cb_110),
    E("nvnCommandBufferSetScissor", cb_111),
    E("nvnCommandBufferSetScissors", cb_112),
    E("nvnCommandBufferSetShaderScratchMemory", cb_113),
    E("nvnCommandBufferSetStencilMask", cb_114),
    E("nvnCommandBufferSetStencilRef", cb_115),
    E("nvnCommandBufferSetStencilValueMask", cb_116),
    E("nvnCommandBufferSetSubpixelPrecisionBias", cb_117),
    E("nvnCommandBufferSetTexturePool", cb_118),
    E("nvnCommandBufferSetTiledCacheAction", cb_119),
    E("nvnCommandBufferSetTiledCacheTileSize", cb_120),
    E("nvnCommandBufferSetViewport", cb_121),
    E("nvnCommandBufferSetViewportSwizzles", cb_122),
    E("nvnCommandBufferSetViewports", cb_123),
    E("nvnCommandBufferTiledDownsample", cb_124),
    E("nvnCommandBufferUpdateUniformBuffer", cb_125),
    E("nvnCommandBufferWaitSync", cb_126),
};
#undef E

void *dm_nvn_cb_lookup(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < 127; i++) if (!strcmp(g_cb_tab[i].name, name)) return g_cb_tab[i].fn;
    return NULL;
}
int dm_nvn_cb_count(void) { return 127; }

// 摘要：把"这个 CB 记了哪些命令、各几次"写进 out（队列提交时打点用）
void dm_nvn_cb_summary(void *cb, char *out, int cap) {
    cb_obj *o = cb_get(cb, 0);
    if (!o || !out || cap <= 0) { if (out && cap > 0) out[0] = 0; return; }
    int n = snprintf(out, (size_t)cap, "ops=%u 内存(cmd %lluB/ctl %lluB) 前几类:",
                     o->nops, (unsigned long long)o->cmd_mem, (unsigned long long)o->ctl_mem);
    int shown = 0;
    for (int i = 0; i < 127 && shown < 6; i++) {
        if (!o->per_id[i]) continue;
        n += snprintf(out + n, (size_t)(cap - n), " %s×%u", g_cb_names[i], o->per_id[i]);
        shown++;
    }
}

// 自测：Begin→记命令→End→摘要；容量查询不为 0
int dm_nvn_cb_selftest(void) {
    int fail = 0;
    static uint64_t cbm[64];
    uint64_t cb = (uint64_t)(uintptr_t)&cbm[0];
    if (cb_Initialize(cb, 0, 0, 0, 0) != 0) fail++;
    if (cb_BeginRecording(cb, 0, 0, 0, 0) != 0) fail++;
    if (cb_IsRecording(cb, 0, 0, 0, 0) != 1) fail++;
    // 记一条 Draw：**通过真实 handler**（这样 per_id 记到的是它自己的下标，摘要里能打出真名）。
    // ⚠ 这里原来写的是 `if (dm_nvn_cb_lookup(...)) fail++;` —— 查到了反而记失败，是自测自身的 bug
    //   （真机第一次跑就报 cb=1，正好把这行揪出来）。已改成查不到才算失败。
    void *fdraw = dm_nvn_cb_lookup("nvnCommandBufferDrawArrays");
    if (!fdraw) fail++;
    else ((uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t))fdraw)(cb, 0, 3, 0, 0);
    if (cb_EndRecording(cb, 0, 0, 0, 0) != 0) fail++;
    if (cb_IsRecording(cb, 0, 0, 0, 0) != 0) fail++;
    if (cb_AddCommandMemory(cb, 0x1000, 0, 0, 0) != 0) fail++;
    if (cb_CmdSize(cb, 0, 0, 0, 0) != 0x1000) fail++;
    if (cb_CmdUsed(cb, 0, 0, 0, 0) != 16) fail++;
    char buf[192];
    dm_nvn_cb_summary((void *)(uintptr_t)cb, buf, (int)sizeof buf);
    if (!strstr(buf, "ops=1")) fail++;
    return fail;
}

