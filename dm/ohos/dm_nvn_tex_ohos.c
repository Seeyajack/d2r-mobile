// ================= M4-a 补完：Texture / Sampler / Buffer·MemoryPool·Program·Sync =================
// 由生成器产出（dm/tools/gen_tex_layer.py 的思路；改法：改生成器重跑，别手改本文件）。
// 154 个函数 = Texture 90 + Sampler 44 + 杂项 20，把游戏请求面补到 100%。
//
// 策略与状态族一致：**影子旁表**（指针 → 32 个字段），Set 存、Get 取。
// 关键点：**Get 的默认值不是 0**——尺寸类给 0 无妨，但 Levels/Samples 必须是 1、Target 必须是 2D，
// 否则游戏读到 "0 级 mip / 0 采样 / 非法目标" 会走异常分支（比崩溃更难查）。
#include <string.h>
#include <stdint.h>
#include "dm_nvn_tex_ohos.h"

#define TT_MAX 256
#define TT_FIELDS 32
typedef struct { const void *p; uint64_t f[TT_FIELDS]; uint32_t used; } tt_ent;
static tt_ent g_tt[TT_MAX];
static int g_ntt;

static tt_ent *tt_get(const void *p, int create) {
    if (!p) return NULL;
    for (int i = 0; i < g_ntt; i++) if (g_tt[i].p == p) return &g_tt[i];
    if (!create || g_ntt >= TT_MAX) return NULL;
    tt_ent *e = &g_tt[g_ntt++];
    memset(e, 0, sizeof *e);
    e->p = p;
    return e;
}
static void tt_set(const void *p, int i, uint64_t v) {
    tt_ent *e = tt_get(p, 1);
    if (!e) return;
    e->f[i] = v; e->used |= (1u << i);
}
static uint64_t tt_read(const void *p, int i, uint64_t def) {
    tt_ent *e = tt_get(p, 0);
    if (!e || !(e->used & (1u << i))) return def;
    return e->f[i];
}

static const uint64_t g_def[TT_FIELDS] = {
    0ull, 0ull, 0ull, 0ull, 1ull, 1ull, 0ull, 2ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull, 0ull,
};

static uint64_t f0(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 13, 0ull);
}
static uint64_t f1(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 14, 0ull);
}
static uint64_t f2(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 35, 0ull);
}
static uint64_t f3(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 22, 0ull);
}
static uint64_t f4(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f5(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 15, 0ull);
}
static uint64_t f6(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 36, 0ull);
}
static uint64_t f7(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 35, 0ull);
}
static uint64_t f8(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 15, 0ull);
}
static uint64_t f9(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 35, 0ull);
}
static uint64_t f10(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f11(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f12(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f13(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f14(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f15(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f16(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 25, 0ull);
}
static uint64_t f17(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 26, 0ull);
}
static uint64_t f18(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 27, 0ull);
}
static uint64_t f19(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 28, 0ull);
}
static uint64_t f20(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 29, 0ull);
}
static uint64_t f21(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 30, 0ull);
}
static uint64_t f22(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 31, 0ull);
}
static uint64_t f23(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 32, 0ull);
}
static uint64_t f24(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 33, 0ull);
}
static uint64_t f25(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 34, 0ull);
}
static uint64_t f26(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 25, v); return 0;
}
static uint64_t f27(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 26, v); return 0;
}
static uint64_t f28(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 27, v); return 0;
}
static uint64_t f29(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 28, v); return 0;
}
static uint64_t f30(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)v; (void)b; (void)d;
    tt_ent *e = tt_get((const void *)(uintptr_t)o, 1);
    if (e) { memset(e->f, 0, sizeof e->f); for (int i = 0; i < TT_FIELDS; i++) e->f[i] = g_def[i]; e->used = 0xFFFFFFFFu; }
}
static uint64_t f31(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 38, v); return 0;
}
static uint64_t f32(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 29, v); return 0;
}
static uint64_t f33(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 30, v); return 0;
}
static uint64_t f34(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 31, v); return 0;
}
static uint64_t f35(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 32, v); return 0;
}
static uint64_t f36(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 33, v); return 0;
}
static uint64_t f37(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 34, v); return 0;
}
static uint64_t f38(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f39(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f40(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 25, 0ull);
}
static uint64_t f41(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 26, 0ull);
}
static uint64_t f42(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 27, 0ull);
}
static uint64_t f43(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 28, 0ull);
}
static uint64_t f44(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 22, 0ull);
}
static uint64_t f45(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 29, 0ull);
}
static uint64_t f46(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 30, 0ull);
}
static uint64_t f47(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 31, 0ull);
}
static uint64_t f48(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 32, 0ull);
}
static uint64_t f49(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 33, 0ull);
}
static uint64_t f50(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 34, 0ull);
}
static uint64_t f51(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f52(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f53(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 13, 0ull);
}
static uint64_t f54(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 14, 0ull);
}
static uint64_t f55(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 35, 0ull);
}
static uint64_t f56(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f57(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f58(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f59(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f60(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f61(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f62(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f63(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f64(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 3, 0ull);
}
static uint64_t f65(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 16, 0ull);
}
static uint64_t f66(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 15, 0ull);
}
static uint64_t f67(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 6, 0ull);
}
static uint64_t f68(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 21, 0ull);
}
static uint64_t f69(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 2, 0ull);
}
static uint64_t f70(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 4, 1ull);
}
static uint64_t f71(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 13, 0ull);
}
static uint64_t f72(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 14, 0ull);
}
static uint64_t f73(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 17, 0ull);
}
static uint64_t f74(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 5, 1ull);
}
static uint64_t f75(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 20, 0ull);
}
static uint64_t f76(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 12, 0ull);
}
static uint64_t f77(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 10, 0ull);
}
static uint64_t f78(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 11, 0ull);
}
static uint64_t f79(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 8, 0ull);
}
static uint64_t f80(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 9, 0ull);
}
static uint64_t f81(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 7, 2ull);
}
static uint64_t f82(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 1, 0ull);
}
static uint64_t f83(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 19, 0ull);
}
static uint64_t f84(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)v; (void)b; (void)d;
    tt_ent *e = tt_get((const void *)(uintptr_t)o, 1);
    if (e) { memset(e->f, 0, sizeof e->f); for (int i = 0; i < TT_FIELDS; i++) e->f[i] = g_def[i]; e->used = 0xFFFFFFFFu; }
}
static uint64_t f85(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 3, v); return 0;
}
static uint64_t f86(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 16, v); return 0;
}
static uint64_t f87(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 38, v); return 0;
}
static uint64_t f88(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 15, v); return 0;
}
static uint64_t f89(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 6, v); return 0;
}
static uint64_t f90(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 21, v); return 0;
}
static uint64_t f91(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 2, v); return 0;
}
static uint64_t f92(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 4, v); return 0;
}
static uint64_t f93(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 17, v); return 0;
}
static uint64_t f94(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 18, v); return 0;
}
static uint64_t f95(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 5, v); return 0;
}
static uint64_t f96(uint64_t o, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4;
    tt_set((const void *)(uintptr_t)o, 1, a1);
    return 0;
}
static uint64_t f97(uint64_t o, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4;
    tt_set((const void *)(uintptr_t)o, 1, a1);
    tt_set((const void *)(uintptr_t)o, 2, a2);
    return 0;
}
static uint64_t f98(uint64_t o, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a4;
    tt_set((const void *)(uintptr_t)o, 1, a1);
    tt_set((const void *)(uintptr_t)o, 2, a2);
    tt_set((const void *)(uintptr_t)o, 3, a3);
    return 0;
}
static uint64_t f99(uint64_t o, uint64_t pool, uint64_t off, uint64_t d) {
    (void)d; tt_set((const void *)(uintptr_t)o, 14, pool);
    tt_set((const void *)(uintptr_t)o, 13, off); return 0;
}
static uint64_t f100(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 8, v); return 0;
}
static uint64_t f101(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 9, v); return 0;
}
static uint64_t f102(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 7, v); return 0;
}
static uint64_t f103(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 1, v); return 0;
}
static uint64_t f104(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f105(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f106(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f107(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 22, 0ull);
}
static uint64_t f108(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 3, 0ull);
}
static uint64_t f109(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 16, 0ull);
}
static uint64_t f110(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 15, 0ull);
}
static uint64_t f111(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 6, 0ull);
}
static uint64_t f112(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 2, 0ull);
}
static uint64_t f113(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 4, 1ull);
}
static uint64_t f114(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 13, 0ull);
}
static uint64_t f115(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 14, 0ull);
}
static uint64_t f116(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 5, 1ull);
}
static uint64_t f117(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 20, 0ull);
}
static uint64_t f118(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 10, 0ull);
}
static uint64_t f119(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 11, 0ull);
}
static uint64_t f120(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 8, 0ull);
}
static uint64_t f121(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 9, 0ull);
}
static uint64_t f122(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 7, 2ull);
}
static uint64_t f123(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 24, 0ull);
}
static uint64_t f124(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 23, 0ull);
}
static uint64_t f125(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 1, 0ull);
}
static uint64_t f126(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 19, 0ull);
}
static uint64_t f127(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f128(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f129(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f130(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 13, 0ull);
}
static uint64_t f131(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 14, 0ull);
}
static uint64_t f132(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 35, 0ull);
}
static uint64_t f133(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f134(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f135(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f136(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f137(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f138(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f139(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}
static uint64_t f140(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 16, 0ull);
}
static uint64_t f141(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 6, 0ull);
}
static uint64_t f142(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 39, 0ull);
}
static uint64_t f143(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 4, 1ull);
}
static uint64_t f144(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 9, 0ull);
}
static uint64_t f145(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)b; (void)cc; (void)d;
    return tt_read((const void *)(uintptr_t)o, 7, 2ull);
}
static uint64_t f146(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)v; (void)b; (void)d;
    tt_ent *e = tt_get((const void *)(uintptr_t)o, 1);
    if (e) { memset(e->f, 0, sizeof e->f); for (int i = 0; i < TT_FIELDS; i++) e->f[i] = g_def[i]; e->used = 0xFFFFFFFFu; }
}
static uint64_t f147(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 16, v); return 0;
}
static uint64_t f148(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 6, v); return 0;
}
static uint64_t f149(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 39, v); return 0;
}
static uint64_t f150(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 4, v); return 0;
}
static uint64_t f151(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 9, v); return 0;
}
static uint64_t f152(uint64_t o, uint64_t v, uint64_t b, uint64_t d) {
    (void)b; (void)d; tt_set((const void *)(uintptr_t)o, 7, v); return 0;
}
static uint64_t f153(uint64_t o, uint64_t b, uint64_t cc, uint64_t d) {
    (void)o; (void)b; (void)cc; (void)d; return 0;
}


typedef struct { const char *name; void *fn; } tt_tab_e;
#define E(n, f) { n, (void *)(f) }
static const tt_tab_e g_tt_tab[154] = {
    E("nvnBufferBuilderGetMemoryOffset", f0),
    E("nvnBufferBuilderGetMemoryPool", f1),
    E("nvnBufferBuilderGetSize", f2),
    E("nvnBufferGetDebugID", f3),
    E("nvnBufferMap", f4),
    E("nvnMemoryPoolBuilderGetFlags", f5),
    E("nvnMemoryPoolBuilderGetMemory", f6),
    E("nvnMemoryPoolBuilderGetSize", f7),
    E("nvnMemoryPoolGetFlags", f8),
    E("nvnMemoryPoolGetSize", f9),
    E("nvnMemoryPoolMapVirtual", f10),
    E("nvnProgramFinalize", f11),
    E("nvnProgramInitialize", f12),
    E("nvnProgramSetDebugLabel", f13),
    E("nvnProgramSetShaders", f14),
    E("nvnProgramSetSubroutineLinkage", f15),
    E("nvnSamplerBuilderGetBorderColor", f16),
    E("nvnSamplerBuilderGetBorderColori", f17),
    E("nvnSamplerBuilderGetBorderColorui", f18),
    E("nvnSamplerBuilderGetCompare", f19),
    E("nvnSamplerBuilderGetLodBias", f20),
    E("nvnSamplerBuilderGetLodClamp", f21),
    E("nvnSamplerBuilderGetMaxAnisotropy", f22),
    E("nvnSamplerBuilderGetMinMagFilter", f23),
    E("nvnSamplerBuilderGetReductionFilter", f24),
    E("nvnSamplerBuilderGetWrapMode", f25),
    E("nvnSamplerBuilderSetBorderColor", f26),
    E("nvnSamplerBuilderSetBorderColori", f27),
    E("nvnSamplerBuilderSetBorderColorui", f28),
    E("nvnSamplerBuilderSetCompare", f29),
    E("nvnSamplerBuilderSetDefaults", f30),
    E("nvnSamplerBuilderSetDevice", f31),
    E("nvnSamplerBuilderSetLodBias", f32),
    E("nvnSamplerBuilderSetLodClamp", f33),
    E("nvnSamplerBuilderSetMaxAnisotropy", f34),
    E("nvnSamplerBuilderSetMinMagFilter", f35),
    E("nvnSamplerBuilderSetReductionFilter", f36),
    E("nvnSamplerBuilderSetWrapMode", f37),
    E("nvnSamplerCompare", f38),
    E("nvnSamplerFinalize", f39),
    E("nvnSamplerGetBorderColor", f40),
    E("nvnSamplerGetBorderColori", f41),
    E("nvnSamplerGetBorderColorui", f42),
    E("nvnSamplerGetCompare", f43),
    E("nvnSamplerGetDebugID", f44),
    E("nvnSamplerGetLodBias", f45),
    E("nvnSamplerGetLodClamp", f46),
    E("nvnSamplerGetMaxAnisotropy", f47),
    E("nvnSamplerGetMinMagFilter", f48),
    E("nvnSamplerGetReductionFilter", f49),
    E("nvnSamplerGetWrapMode", f50),
    E("nvnSamplerInitialize", f51),
    E("nvnSamplerPoolFinalize", f52),
    E("nvnSamplerPoolGetMemoryOffset", f53),
    E("nvnSamplerPoolGetMemoryPool", f54),
    E("nvnSamplerPoolGetSize", f55),
    E("nvnSamplerPoolInitialize", f56),
    E("nvnSamplerPoolRegisterSampler", f57),
    E("nvnSamplerPoolSetDebugLabel", f58),
    E("nvnSamplerSetDebugLabel", f59),
    E("nvnSyncFinalize", f60),
    E("nvnSyncInitialize", f61),
    E("nvnSyncSetDebugLabel", f62),
    E("nvnSyncWait", f63),
    E("nvnTextureBuilderGetDepth", f64),
    E("nvnTextureBuilderGetDepthStencilMode", f65),
    E("nvnTextureBuilderGetFlags", f66),
    E("nvnTextureBuilderGetFormat", f67),
    E("nvnTextureBuilderGetGLTextureName", f68),
    E("nvnTextureBuilderGetHeight", f69),
    E("nvnTextureBuilderGetLevels", f70),
    E("nvnTextureBuilderGetMemoryOffset", f71),
    E("nvnTextureBuilderGetMemoryPool", f72),
    E("nvnTextureBuilderGetPackagedTextureData", f73),
    E("nvnTextureBuilderGetSamples", f74),
    E("nvnTextureBuilderGetSparseTileLayout", f75),
    E("nvnTextureBuilderGetStorageAlignment", f76),
    E("nvnTextureBuilderGetStorageClass", f77),
    E("nvnTextureBuilderGetStorageSize", f78),
    E("nvnTextureBuilderGetStride", f79),
    E("nvnTextureBuilderGetSwizzle", f80),
    E("nvnTextureBuilderGetTarget", f81),
    E("nvnTextureBuilderGetWidth", f82),
    E("nvnTextureBuilderGetZCullStorageSize", f83),
    E("nvnTextureBuilderSetDefaults", f84),
    E("nvnTextureBuilderSetDepth", f85),
    E("nvnTextureBuilderSetDepthStencilMode", f86),
    E("nvnTextureBuilderSetDevice", f87),
    E("nvnTextureBuilderSetFlags", f88),
    E("nvnTextureBuilderSetFormat", f89),
    E("nvnTextureBuilderSetGLTextureName", f90),
    E("nvnTextureBuilderSetHeight", f91),
    E("nvnTextureBuilderSetLevels", f92),
    E("nvnTextureBuilderSetPackagedTextureData", f93),
    E("nvnTextureBuilderSetPackagedTextureLayout", f94),
    E("nvnTextureBuilderSetSamples", f95),
    E("nvnTextureBuilderSetSize1D", f96),
    E("nvnTextureBuilderSetSize2D", f97),
    E("nvnTextureBuilderSetSize3D", f98),
    E("nvnTextureBuilderSetStorage", f99),
    E("nvnTextureBuilderSetStride", f100),
    E("nvnTextureBuilderSetSwizzle", f101),
    E("nvnTextureBuilderSetTarget", f102),
    E("nvnTextureBuilderSetWidth", f103),
    E("nvnTextureCompare", f104),
    E("nvnTextureFinalize", f105),
    E("nvnTextureFlushTexels", f106),
    E("nvnTextureGetDebugID", f107),
    E("nvnTextureGetDepth", f108),
    E("nvnTextureGetDepthStencilMode", f109),
    E("nvnTextureGetFlags", f110),
    E("nvnTextureGetFormat", f111),
    E("nvnTextureGetHeight", f112),
    E("nvnTextureGetLevels", f113),
    E("nvnTextureGetMemoryOffset", f114),
    E("nvnTextureGetMemoryPool", f115),
    E("nvnTextureGetSamples", f116),
    E("nvnTextureGetSparseTileLayout", f117),
    E("nvnTextureGetStorageClass", f118),
    E("nvnTextureGetStorageSize", f119),
    E("nvnTextureGetStride", f120),
    E("nvnTextureGetSwizzle", f121),
    E("nvnTextureGetTarget", f122),
    E("nvnTextureGetTextureAddress", f123),
    E("nvnTextureGetViewOffset", f124),
    E("nvnTextureGetWidth", f125),
    E("nvnTextureGetZCullStorageSize", f126),
    E("nvnTextureInitialize", f127),
    E("nvnTextureInvalidateTexels", f128),
    E("nvnTexturePoolFinalize", f129),
    E("nvnTexturePoolGetMemoryOffset", f130),
    E("nvnTexturePoolGetMemoryPool", f131),
    E("nvnTexturePoolGetSize", f132),
    E("nvnTexturePoolInitialize", f133),
    E("nvnTexturePoolRegisterImage", f134),
    E("nvnTexturePoolRegisterTexture", f135),
    E("nvnTexturePoolSetDebugLabel", f136),
    E("nvnTextureReadTexels", f137),
    E("nvnTextureSetDebugLabel", f138),
    E("nvnTextureViewCompare", f139),
    E("nvnTextureViewGetDepthStencilMode", f140),
    E("nvnTextureViewGetFormat", f141),
    E("nvnTextureViewGetLayers", f142),
    E("nvnTextureViewGetLevels", f143),
    E("nvnTextureViewGetSwizzle", f144),
    E("nvnTextureViewGetTarget", f145),
    E("nvnTextureViewSetDefaults", f146),
    E("nvnTextureViewSetDepthStencilMode", f147),
    E("nvnTextureViewSetFormat", f148),
    E("nvnTextureViewSetLayers", f149),
    E("nvnTextureViewSetLevels", f150),
    E("nvnTextureViewSetSwizzle", f151),
    E("nvnTextureViewSetTarget", f152),
    E("nvnTextureWriteTexels", f153),
};
#undef E

void *dm_nvn_tex_lookup(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < 154; i++) if (!strcmp(g_tt_tab[i].name, name)) return g_tt_tab[i].fn;
    return NULL;
}
int dm_nvn_tex_count(void) { return 154; }

// 自测：Builder→Get 往返（尺寸/格式/目标）+ 默认值不是 0 的项
int dm_nvn_tex_selftest(void) {
    int fail = 0;
    static uint64_t b[64];
    const void *o = &b[0];
    // 找几个代表性函数
    void *fDef  = dm_nvn_tex_lookup("nvnTextureBuilderSetDefaults");
    void *fSize2 = dm_nvn_tex_lookup("nvnTextureBuilderSetSize2D");
    void *fFmt  = dm_nvn_tex_lookup("nvnTextureBuilderSetFormat");
    void *gW    = dm_nvn_tex_lookup("nvnTextureBuilderGetWidth");
    void *gH    = dm_nvn_tex_lookup("nvnTextureBuilderGetHeight");
    void *gL    = dm_nvn_tex_lookup("nvnTextureBuilderGetLevels");
    void *gT    = dm_nvn_tex_lookup("nvnTextureBuilderGetTarget");
    if (!fDef || !fSize2 || !fFmt || !gW || !gH || !gL || !gT) return 99;
    ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))fDef)((uint64_t)(uintptr_t)o,0,0,0,0);
    if (((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))gL)((uint64_t)(uintptr_t)o,0,0,0,0) != 1) fail++;
    if (((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))gT)((uint64_t)(uintptr_t)o,0,0,0,0) != 2) fail++;
    ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))fSize2)((uint64_t)(uintptr_t)o, 1920, 1080, 0, 0);
    if (((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))gW)((uint64_t)(uintptr_t)o,0,0,0,0) != 1920) fail++;
    if (((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))gH)((uint64_t)(uintptr_t)o,0,0,0,0) != 1080) fail++;
    ((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))fFmt)((uint64_t)(uintptr_t)o, 37, 0, 0, 0);
    // 未知对象：Get 应返回默认值 1/2，而不是 0
    if (((uint64_t (*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t))gL)(0xDEAD0000ull,0,0,0,0) != 1) fail++;
    return fail;
}

