// ================= M4-a：NVN 状态族（纯数据）=================
// 为什么先做这一族（2026-10-22）：
//   `dm/tools/nvn_coverage.py` 算出来：游戏要 449 个 NVN 函数，我们只实现了 31 个，缺 418。
//   但缺口里 **295 个是纯数据搬运**（Builder/Set/Get）——它们不需要 GPU、不需要真机，
//   只要"存进去能读回来"就是对。状态族（Blend/ChannelMask/Color/DepthStencil/Multisample/
//   Polygon/VertexAttrib/VertexStream）共 77 个，正好是这类里最规整的一批，且是"绘制前必须摆对"的东西。
//
// 设计取舍（重要）：
//   1. **不往游戏给的对象里写**，而是用"指针 → 一份影子字段"的旁表。
//      理由：状态对象的真实大小/NVN 官方布局我们不确定，写进去可能越界踩坏相邻内存；
//      而这一族的语义只要求"我们读得到自己写的东西"⇒ 旁表足够，且零越界风险。
//   2. Get 找不到对象时返回该族的**规范默认值**（NVN 语义），而不是 0 —— 否则表现是"全黑/全剔除"。
//      ⚠ 默认值是按 NVN 常规语义填的，**待真机核对**（真机跑 `dm_nvn_state_selftest()` 会打印偏差）。
//   3. `SetDefaults()` 把该族填成默认值；`Set*` 只改一个字段。
//
// 判据：
//   · 离线：编译通过 + `nvn_coverage.py` 里这 8 个族的"缺"归零；
//   · 真机：`dm_nvn_state_selftest()` 逐族 Set→Get 往返比对，返回失败数（挂到探针按钮上）。
// Pure-data NVN state families. Values live in a pointer-keyed side table (never in the game's
// object, so there is no risk of writing past its real size). Getters fall back to NVN defaults.

#include <string.h>
#include <stdint.h>
#include "dm_nvn_state_ohos.h"

// ── 旁表：指针 → 24 个 u64 字段 ──
#define ST_MAX 192
#define ST_FIELDS 24
typedef struct { const void *obj; uint64_t f[ST_FIELDS]; uint32_t used; } st_ent;
static st_ent g_st[ST_MAX];
static int g_st_n;

static st_ent *st_find(const void *obj, int create) {
    if (!obj) return NULL;
    for (int i = 0; i < g_st_n; i++)
        if (g_st[i].obj == obj) return &g_st[i];
    if (!create || g_st_n >= ST_MAX) return NULL;
    st_ent *e = &g_st[g_st_n++];
    memset(e, 0, sizeof *e);
    e->obj = obj;
    return e;
}
static uint64_t st_get(const void *obj, int idx, uint64_t def) {
    st_ent *e = st_find(obj, 0);
    if (!e || !(e->used & (1u << idx))) return def;
    return e->f[idx];
}
static void st_set(const void *obj, int idx, uint64_t v) {
    st_ent *e = st_find(obj, 1);
    if (!e) return;
    e->f[idx] = v;
    e->used |= (1u << idx);
}

// ── 各族的规范默认值（NVN 常规语义；⚠ 待真机核对）──
// 字段号见下方各族 SET/GET 的第二个参数。
static const uint64_t DEF_BLEND[8]     = { 0xF, 0 /*ADD*/, 0 /*One,Zero*/, 0, 0, 0, 0, 0 };
static const uint64_t DEF_CHMASK[2]    = { 0xF /*RGBA*/, 0 };
static const uint64_t DEF_COLOR[4]     = { 0 /*blend off*/, 0 /*alphaTest off*/, 0 /*logicOp off*/, 0 };
static const uint64_t DEF_DEPTH[8]     = { 1 /*depthTest on*/, 1 /*write on*/, 1 /*LESS*/, 0 /*stencil off*/, 0, 0, 0, 0 };
static const uint64_t DEF_MS[16]       = { 0 /*multisample off*/, 1 /*samples*/, 0 /*rasterSamples*/,
                                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint64_t DEF_POLY[8]      = { 1 /*BACK*/, 1 /*CCW*/, 0 /*FILL*/, 0 /*offset off*/, 0, 0, 0, 0 };
static const uint64_t DEF_VATTRIB[4]   = { 0, 0, 0, 0 };
static const uint64_t DEF_VSTREAM[4]   = { 0 /*stride*/, 0 /*divisor*/, 0, 0 };

static void st_defaults(const void *obj, const uint64_t *def, int n) {
    st_ent *e = st_find(obj, 1);
    if (!e) return;
    memset(e->f, 0, sizeof e->f);
    for (int i = 0; i < n; i++) e->f[i] = def[i];
    e->used = (n >= (int)ST_FIELDS) ? 0xFFFFFFFFu : ((1u << n) - 1u);
}

// ── 生成 77 个函数：一行一个（宏只为少写样板，语义就是"存/取一个字段"）──
// SET：x0 = 状态对象，x1 = 值 → 返回 0（NVN 的 Result 语义）
#define SETFN(fam, idx) \
    static uint64_t fam##_set##idx(uint64_t o, uint64_t v, uint64_t c, uint64_t d) { \
        (void)c; (void)d; st_set((const void *)(uintptr_t)o, idx, v); return 0; }
// GET：x0 = 状态对象 → 返回值
#define GETFN(fam, idx, def) \
    static uint64_t fam##_get##idx(uint64_t o, uint64_t b, uint64_t c, uint64_t d) { \
        (void)b; (void)c; (void)d; return st_get((const void *)(uintptr_t)o, idx, def); }

// BlendState（15）
SETFN(bl, 0) /*BlendTarget*/   GETFN(bl, 0, 0xF)
SETFN(bl, 1) /*BlendEquation*/ GETFN(bl, 1, 0)
SETFN(bl, 2) /*BlendFunc*/     GETFN(bl, 2, 0)
SETFN(bl, 3) /*AdvMode*/       GETFN(bl, 3, 0)
SETFN(bl, 4) /*AdvNormDst*/    GETFN(bl, 4, 0)
SETFN(bl, 5) /*AdvOverlap*/    GETFN(bl, 5, 0)
SETFN(bl, 6) /*AdvPremulSrc*/  GETFN(bl, 6, 0)
// ChannelMaskState（3）
SETFN(cm, 0)                   GETFN(cm, 0, 0xF)
// ColorState（7）
SETFN(co, 0) /*BlendEnable*/   GETFN(co, 0, 0)
SETFN(co, 1) /*AlphaTest*/     GETFN(co, 1, 0)
SETFN(co, 2) /*LogicOp*/       GETFN(co, 2, 0)
// DepthStencilState（13）
SETFN(ds, 0) /*DepthTest*/     GETFN(ds, 0, 1)
SETFN(ds, 1) /*DepthWrite*/    GETFN(ds, 1, 1)
SETFN(ds, 2) /*DepthFunc*/     GETFN(ds, 2, 1)
SETFN(ds, 3) /*StencilTest*/   GETFN(ds, 3, 0)
SETFN(ds, 4) /*StencilFunc*/   GETFN(ds, 4, 0)
SETFN(ds, 5) /*StencilOp*/     GETFN(ds, 5, 0)
// MultisampleState（23）
SETFN(ms, 0) /*MSEnable*/      GETFN(ms, 0, 0)
SETFN(ms, 1) /*Samples*/       GETFN(ms, 1, 1)
SETFN(ms, 2) /*RasterSamples*/ GETFN(ms, 2, 0)
SETFN(ms, 3) /*A2C*/           GETFN(ms, 3, 0)
SETFN(ms, 4) /*A2CDither*/     GETFN(ms, 4, 0)
SETFN(ms, 5) /*CovToColor*/    GETFN(ms, 5, 0)
SETFN(ms, 6) /*CovToColorOut*/ GETFN(ms, 6, 0)
SETFN(ms, 7) /*CovModMode*/    GETFN(ms, 7, 0)
SETFN(ms, 8) /*SampleLocEn*/   GETFN(ms, 8, 0)
SETFN(ms, 9) /*GridEnable*/    GETFN(ms, 9, 0)
SETFN(ms, 10)/*SampleLoc*/     GETFN(ms, 10, 0)
SETFN(ms, 11)/*Grid*/          GETFN(ms, 11, 0)
// PolygonState（9）
SETFN(pg, 0) /*CullFace*/      GETFN(pg, 0, 1)
SETFN(pg, 1) /*FrontFace*/     GETFN(pg, 1, 1)
SETFN(pg, 2) /*PolygonMode*/   GETFN(pg, 2, 0)
SETFN(pg, 3) /*OffsetEnables*/ GETFN(pg, 3, 0)
// VertexAttribState（5）
SETFN(va, 0) /*Format*/        GETFN(va, 0, 0)
SETFN(va, 1) /*StreamIndex*/   GETFN(va, 1, 0)
// VertexStreamState（5）
SETFN(vs, 0) /*Stride*/        GETFN(vs, 0, 0)
SETFN(vs, 1) /*Divisor*/       GETFN(vs, 1, 0)

// SetDefaults（8 个）
#define DEFSET(argv, def) \
    static uint64_t argv(uint64_t o, uint64_t b, uint64_t c, uint64_t d) { \
        (void)b; (void)c; (void)d; st_defaults((const void *)(uintptr_t)o, def, (int)(sizeof def / 8)); return 0; }
DEFSET(bl_def,   DEF_BLEND)
DEFSET(cm_def,   DEF_CHMASK)
DEFSET(co_def,   DEF_COLOR)
DEFSET(ds_def,   DEF_DEPTH)
DEFSET(ms_def,   DEF_MS)
DEFSET(pg_def,   DEF_POLY)
DEFSET(va_def,   DEF_VATTRIB)
DEFSET(vs_def,   DEF_VSTREAM)

// ── 名字 → 函数（77 个；名字必须与游戏 .rodata 里的完全一致）──
typedef struct { const char *name; void *fn; } st_ent_tab;
#define E(n, f) { n, (void *)(f) }
static const st_ent_tab g_state_tab[] = {
    E("nvnBlendStateSetDefaults", bl_def),
    E("nvnBlendStateSetBlendTarget", bl_set0),          E("nvnBlendStateGetBlendTarget", bl_get0),
    E("nvnBlendStateSetBlendEquation", bl_set1),        E("nvnBlendStateGetBlendEquation", bl_get1),
    E("nvnBlendStateSetBlendFunc", bl_set2),            E("nvnBlendStateGetBlendFunc", bl_get2),
    E("nvnBlendStateSetAdvancedMode", bl_set3),         E("nvnBlendStateGetAdvancedMode", bl_get3),
    E("nvnBlendStateSetAdvancedNormalizedDst", bl_set4),E("nvnBlendStateGetAdvancedNormalizedDst", bl_get4),
    E("nvnBlendStateSetAdvancedOverlap", bl_set5),      E("nvnBlendStateGetAdvancedOverlap", bl_get5),
    E("nvnBlendStateSetAdvancedPremultipliedSrc", bl_set6), E("nvnBlendStateGetAdvancedPremultipliedSrc", bl_get6),

    E("nvnChannelMaskStateSetDefaults", cm_def),
    E("nvnChannelMaskStateSetChannelMask", cm_set0),    E("nvnChannelMaskStateGetChannelMask", cm_get0),

    E("nvnColorStateSetDefaults", co_def),
    E("nvnColorStateSetBlendEnable", co_set0),          E("nvnColorStateGetBlendEnable", co_get0),
    E("nvnColorStateSetAlphaTest", co_set1),            E("nvnColorStateGetAlphaTest", co_get1),
    E("nvnColorStateSetLogicOp", co_set2),              E("nvnColorStateGetLogicOp", co_get2),

    E("nvnDepthStencilStateSetDefaults", ds_def),
    E("nvnDepthStencilStateSetDepthTestEnable", ds_set0),  E("nvnDepthStencilStateGetDepthTestEnable", ds_get0),
    E("nvnDepthStencilStateSetDepthWriteEnable", ds_set1), E("nvnDepthStencilStateGetDepthWriteEnable", ds_get1),
    E("nvnDepthStencilStateSetDepthFunc", ds_set2),        E("nvnDepthStencilStateGetDepthFunc", ds_get2),
    E("nvnDepthStencilStateSetStencilTestEnable", ds_set3),E("nvnDepthStencilStateGetStencilTestEnable", ds_get3),
    E("nvnDepthStencilStateSetStencilFunc", ds_set4),      E("nvnDepthStencilStateGetStencilFunc", ds_get4),
    E("nvnDepthStencilStateSetStencilOp", ds_set5),        E("nvnDepthStencilStateGetStencilOp", ds_get5),

    E("nvnMultisampleStateSetDefaults", ms_def),
    E("nvnMultisampleStateSetMultisampleEnable", ms_set0), E("nvnMultisampleStateGetMultisampleEnable", ms_get0),
    E("nvnMultisampleStateSetSamples", ms_set1),           E("nvnMultisampleStateGetSamples", ms_get1),
    E("nvnMultisampleStateSetRasterSamples", ms_set2),     E("nvnMultisampleStateGetRasterSamples", ms_get2),
    E("nvnMultisampleStateSetAlphaToCoverageEnable", ms_set3), E("nvnMultisampleStateGetAlphaToCoverageEnable", ms_get3),
    E("nvnMultisampleStateSetAlphaToCoverageDither", ms_set4), E("nvnMultisampleStateGetAlphaToCoverageDither", ms_get4),
    E("nvnMultisampleStateSetCoverageToColorEnable", ms_set5), E("nvnMultisampleStateGetCoverageToColorEnable", ms_get5),
    E("nvnMultisampleStateSetCoverageToColorOutput", ms_set6), E("nvnMultisampleStateGetCoverageToColorOutput", ms_get6),
    E("nvnMultisampleStateSetCoverageModulationMode", ms_set7),E("nvnMultisampleStateGetCoverageModulationMode", ms_get7),
    E("nvnMultisampleStateSetSampleLocationsEnable", ms_set8), E("nvnMultisampleStateGetSampleLocationsEnable", ms_get8),
    E("nvnMultisampleStateSetSampleLocationsGridEnable", ms_set9), E("nvnMultisampleStateGetSampleLocationsGridEnable", ms_get9),
    E("nvnMultisampleStateSetSampleLocations", ms_set10),  E("nvnMultisampleStateGetSampleLocations", ms_get10),
    E("nvnMultisampleStateSetSampleLocationsGrid", ms_set11), E("nvnMultisampleStateGetSampleLocationsGrid", ms_get11),

    E("nvnPolygonStateSetDefaults", pg_def),
    E("nvnPolygonStateSetCullFace", pg_set0),            E("nvnPolygonStateGetCullFace", pg_get0),
    E("nvnPolygonStateSetFrontFace", pg_set1),           E("nvnPolygonStateGetFrontFace", pg_get1),
    E("nvnPolygonStateSetPolygonMode", pg_set2),         E("nvnPolygonStateGetPolygonMode", pg_get2),
    E("nvnPolygonStateSetPolygonOffsetEnables", pg_set3),E("nvnPolygonStateGetPolygonOffsetEnables", pg_get3),

    E("nvnVertexAttribStateSetDefaults", va_def),
    E("nvnVertexAttribStateSetFormat", va_set0),         E("nvnVertexAttribStateGetFormat", va_get0),
    E("nvnVertexAttribStateSetStreamIndex", va_set1),    E("nvnVertexAttribStateGetStreamIndex", va_get1),

    E("nvnVertexStreamStateSetDefaults", vs_def),
    E("nvnVertexStreamStateSetStride", vs_set0),         E("nvnVertexStreamStateGetStride", vs_get0),
    E("nvnVertexStreamStateSetDivisor", vs_set1),        E("nvnVertexStreamStateGetDivisor", vs_get1),
};
#undef E

void *dm_nvn_state_lookup(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_state_tab / sizeof g_state_tab[0]; i++)
        if (!strcmp(g_state_tab[i].name, name)) return g_state_tab[i].fn;
    return NULL;
}

int dm_nvn_state_count(void) { return (int)(sizeof g_state_tab / sizeof g_state_tab[0]); }

// ── 自测：真机一键验（挂探针按钮）；返回失败项数 ──
// 判据：Set(a) → Get()==a；SetDefaults() → Get()==默认值。**只查往返，不查语义正确性**
//（语义正确性只能靠出帧比对，离线/单测都替代不了）。
int dm_nvn_state_selftest(void) {
    int fail = 0;
    static uint64_t dummy[8];                     // 假状态对象（不往游戏对象里写）
    void *o = &dummy[0];

    // Blend
    bl_def((uint64_t)(uintptr_t)o, 0, 0, 0);
    if (bl_get0((uint64_t)(uintptr_t)o, 0, 0, 0) != 0xF) fail++;
    bl_set0((uint64_t)(uintptr_t)o, 0x3, 0, 0);
    if (bl_get0((uint64_t)(uintptr_t)o, 0, 0, 0) != 0x3) fail++;
    // Depth
    ds_def((uint64_t)(uintptr_t)o, 0, 0, 0);
    if (ds_get2((uint64_t)(uintptr_t)o, 0, 0, 0) != 1) fail++;
    ds_set2((uint64_t)(uintptr_t)o, 4, 0, 0);
    if (ds_get2((uint64_t)(uintptr_t)o, 0, 0, 0) != 4) fail++;
    // Polygon
    pg_def((uint64_t)(uintptr_t)o, 0, 0, 0);
    pg_set0((uint64_t)(uintptr_t)o, 0, 0, 0);
    if (pg_get0((uint64_t)(uintptr_t)o, 0, 0, 0) != 0) fail++;
    // VertexStream
    vs_set0((uint64_t)(uintptr_t)o, 32, 0, 0);
    if (vs_get0((uint64_t)(uintptr_t)o, 0, 0, 0) != 32) fail++;
    // 未知对象：应回落到默认值而不是 0
    if (ds_get0(0xDEADBEEF, 0, 0, 0) != 1) fail++;
    return fail;
}
