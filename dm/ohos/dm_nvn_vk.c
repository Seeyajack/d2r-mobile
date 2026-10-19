// 通用 NVN 内存池/缓冲胶水层实现（实现要点见块内注释；此处只说明归属）。
// Implementation. This file was extracted from dm_probe.c (Task #2) so the NVN
// implementations live in the framework, not in the one-off probe.
#include "dm_nvn_vk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void (*g_log)(const char *, ...);
void dm_nvn_vk_set_log(void (*fn)(const char *, ...)) { g_log = fn; }
#define NLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// ===== NVN 内存池 + 缓冲：最小真实现（2026-10-18 凌晨，Task #2，用户已批准）=====
// 真机 np43 的结论（进度文档 §28）：游戏已经真的在调
//   MemoryPool（Builder×4 + Initialize）、Buffer（Builder×3 + Initialize + GetAddress）、
//   Program（Initialize）——共 14/523 个 API。
// 崩点 si_addr=0x8 就是 `nvnBufferGetAddress` 返 0 造成的（游戏往"缓冲+8"写）。
//
// 设计要点（鸿蒙是 UMA：CPU/GPU 同一块内存，所以不需要真显存对象）：
//   · 池：`Initialize` 直接 malloc(size) 备一块，`Map/GetBufferAddress` 返回它；
//   · 缓冲：`Initialize` 也直接 malloc(size)（**不**从池里切——切分容易重叠，先求稳），
//           `GetAddress` 返回它；
//   · Builder 是**游戏自己分配**的结构体，我们不去改它的内存（布局未知），
//     而是用一张小表按 builder 指针记下参数，Initialize 时按指针回查。
//   · Flush/Invalidate 在 UMA 上天然一致 ⇒ no-op。
// 判据：si_addr=0x8 崩点消失，游戏能继续往下走（可见 FlushMappedRange 等旁证）。
// ★ 兜底尺寸（2026-10-18 01:45，用户批准方案 A）：
//   真机 np44 实测游戏传进来的 size 全是 0（因为它要的尺寸来自 GameConfig，
//   而我们的 GameConfig 是空壳——真数据在 romfs 里，宿主没挂载）。
//   这里在 size==0 时给一个默认值，**只为一轮定性**：验证"size=0 是不是唯一障碍"。
//   日志一律标明「兜底」，将来接上资源层后应当自然消失（届时把这几个默认值删掉）。
#define NVN_POOL_DEFAULT_SIZE   (16u << 20)   // 16MB
#define NVN_BUF_DEFAULT_SIZE    (1u << 20)    // 1MB
#define NVN_MP_MAX   8
#define NVN_BUF_MAX  32
typedef struct { const void *builder; void *storage; uint64_t size, flags; } nvn_mp_pend_t;
typedef struct { const void *builder; const void *pool; uint64_t offset, size; } nvn_buf_pend_t;
typedef struct { const void *obj; void *mem; uint64_t size; } nvn_pool_t;
typedef struct { const void *obj; void *addr; uint64_t size, offset; const void *pool; } nvn_buf_t;
static nvn_mp_pend_t  g_mp_pend[NVN_MP_MAX];
static nvn_buf_pend_t g_buf_pend[NVN_BUF_MAX];
static nvn_pool_t     g_pools[NVN_MP_MAX];
static nvn_buf_t      g_bufs[NVN_BUF_MAX];
static int g_n_mp, g_n_buf;

static nvn_mp_pend_t *mp_pend(const void *b, int make) {
    for (int i = 0; i < g_n_mp; i++) if (g_mp_pend[i].builder == b) return &g_mp_pend[i];
    if (!make || g_n_mp >= NVN_MP_MAX) return NULL;
    nvn_mp_pend_t *e = &g_mp_pend[g_n_mp++];
    e->builder = b; e->storage = NULL; e->size = 0; e->flags = 0;
    return e;
}
static nvn_buf_pend_t *buf_pend(const void *b, int make) {
    for (int i = 0; i < g_n_buf; i++) if (g_buf_pend[i].builder == b) return &g_buf_pend[i];
    if (!make || g_n_buf >= NVN_BUF_MAX) return NULL;
    nvn_buf_pend_t *e = &g_buf_pend[g_n_buf++];
    e->builder = b; e->pool = NULL; e->offset = 0; e->size = 0;
    return e;
}
static nvn_pool_t *pool_of(const void *o) {
    for (int i = 0; i < g_n_mp; i++) if (g_pools[i].obj == o) return &g_pools[i];
    return NULL;
}
static nvn_buf_t *buf_of(const void *o) {
    for (int i = 0; i < g_n_buf; i++) if (g_bufs[i].obj == o) return &g_bufs[i];
    return NULL;
}

// ---- MemoryPool builder ----
static void nvn_mpb_set_device(void *b, const void *dev) { (void)dev; mp_pend(b, 1); }
static void nvn_mpb_set_defaults(void *b) { nvn_mp_pend_t *e = mp_pend(b, 1); if (e) { e->size = 0; e->flags = 0; } }
static void nvn_mpb_set_storage(void *b, void *storage, uint64_t size) {
    nvn_mp_pend_t *e = mp_pend(b, 1);
    if (e) { e->storage = storage; e->size = size; }
    NLOG("[NVN] 池 Builder：storage=%p size=%llu\n", storage, (unsigned long long)size);
}
static void nvn_mpb_set_flags(void *b, uint32_t flags) { nvn_mp_pend_t *e = mp_pend(b, 1); if (e) e->flags = flags; }
// ---- MemoryPool ----
static uint64_t nvn_mp_init(void *pool, const void *builder) {
    nvn_mp_pend_t *e = mp_pend(builder, 0);
    uint64_t size = e ? e->size : 0;
    int fallback = 0;
    if (size == 0) { size = NVN_POOL_DEFAULT_SIZE; fallback = 1; }   // ★ 兜底（见上方注释）
    void *mem = e && e->storage ? e->storage : NULL;
    if (!mem) mem = calloc(1, (size_t)size);
    if (g_n_mp < NVN_MP_MAX) { g_pools[g_n_mp].obj = pool; g_pools[g_n_mp].mem = mem; g_pools[g_n_mp].size = size; g_n_mp++; }
    NLOG("[NVN] 池初始化：pool=%p size=%llu%s → 内存=%p（%s）\n",
         pool, (unsigned long long)size, fallback ? "【兜底】" : "",
         mem, mem ? "就绪" : "空");
    return mem ? 1 : 0;                          // nvnboolean
}
static void *nvn_mp_map(const void *pool) { nvn_pool_t *p = pool_of(pool); return p ? p->mem : NULL; }
static void *nvn_mp_addr(const void *pool) { nvn_pool_t *p = pool_of(pool); return p ? p->mem : NULL; }
static void nvn_mp_flush(void *pool, int64_t off, uint64_t size) { (void)pool; (void)off; (void)size; }
static void nvn_mp_inval(void *pool, int64_t off, uint64_t size) { (void)pool; (void)off; (void)size; }
static void nvn_mp_label(void *pool, const char *s) { (void)pool; (void)s; }
static void nvn_mp_finalize(void *pool) { (void)pool; }
// ---- Buffer builder ----
static void nvn_bb_set_device(void *b, const void *dev) { (void)dev; buf_pend(b, 1); }
static void nvn_bb_set_defaults(void *b) { nvn_buf_pend_t *e = buf_pend(b, 1); if (e) { e->size = 0; e->offset = 0; } }
static void nvn_bb_set_storage(void *b, const void *pool, int64_t offset, uint64_t size) {
    nvn_buf_pend_t *e = buf_pend(b, 1);
    if (e) { e->pool = pool; e->offset = (uint64_t)offset; e->size = size; }
    NLOG("[NVN] 缓冲 Builder：pool=%p offset=%lld size=%llu\n",
         pool, (long long)offset, (unsigned long long)size);
}
// ---- Buffer ----
static uint64_t nvn_buf_init(void *buf, const void *builder) {
    nvn_buf_pend_t *e = buf_pend(builder, 0);
    uint64_t size = e ? e->size : 0, off = e ? e->offset : 0;
    const void *pool = e ? e->pool : NULL;
    int fallback = 0;
    if (size == 0) { size = NVN_BUF_DEFAULT_SIZE; fallback = 1; }     // ★ 兜底（见上方注释）
    void *addr = NULL;
    // UMA：优先用池里那块 + 偏移（与真实语义一致）；池里放不下或没有池就单独备一块。
    nvn_pool_t *p = pool ? pool_of(pool) : NULL;
    if (p && p->mem && off + size <= p->size) addr = (uint8_t *)p->mem + off;
    else addr = calloc(1, (size_t)size);
    if (g_n_buf < NVN_BUF_MAX) {
        g_bufs[g_n_buf].obj = buf; g_bufs[g_n_buf].addr = addr;
        g_bufs[g_n_buf].size = size; g_bufs[g_n_buf].offset = off;
        g_bufs[g_n_buf].pool = pool; g_n_buf++;
    }
    NLOG("[NVN] 缓冲初始化：buf=%p size=%llu%s offset=%llu → 地址=%p（%s）\n",
         buf, (unsigned long long)size, fallback ? "【兜底】" : "",
         (unsigned long long)off, addr,
         (p && addr && addr == (uint8_t *)p->mem + off) ? "池内" : "独立");
    return addr ? 1 : 0;
}
static void *nvn_buf_addr(const void *buf) { nvn_buf_t *b = buf_of(buf); return b ? b->addr : NULL; }
static uint64_t nvn_buf_size(const void *buf) { nvn_buf_t *b = buf_of(buf); return b ? b->size : 0; }
static int64_t nvn_buf_offset(const void *buf) { nvn_buf_t *b = buf_of(buf); return b ? (int64_t)b->offset : 0; }
static const void *nvn_buf_pool(const void *buf) { nvn_buf_t *b = buf_of(buf); return b ? b->pool : NULL; }
static void nvn_buf_flush(void *buf, int64_t off, uint64_t size) { (void)buf; (void)off; (void)size; }
static void nvn_buf_inval(void *buf, int64_t off, uint64_t size) { (void)buf; (void)off; (void)size; }
static void nvn_buf_label(void *buf, const char *s) { (void)buf; (void)s; }
static void nvn_buf_finalize(void *buf) { (void)buf; }

// 名字 → 真实现。**必须在记账桩表之前查**，否则会拿回返 0 的桩。
typedef struct { const char *name; void *fn; } nvn_real_t;
static const nvn_real_t g_nvn_real[] = {
    { "nvnMemoryPoolBuilderSetDevice",        (void *)nvn_mpb_set_device },
    { "nvnMemoryPoolBuilderSetDefaults",      (void *)nvn_mpb_set_defaults },
    { "nvnMemoryPoolBuilderSetStorage",       (void *)nvn_mpb_set_storage },
    { "nvnMemoryPoolBuilderSetFlags",         (void *)nvn_mpb_set_flags },
    { "nvnMemoryPoolInitialize",              (void *)nvn_mp_init },
    { "nvnMemoryPoolMap",                     (void *)nvn_mp_map },
    { "nvnMemoryPoolGetBufferAddress",        (void *)nvn_mp_addr },
    { "nvnMemoryPoolFlushMappedRange",        (void *)nvn_mp_flush },
    { "nvnMemoryPoolInvalidateMappedRange",   (void *)nvn_mp_inval },
    { "nvnMemoryPoolSetDebugLabel",           (void *)nvn_mp_label },
    { "nvnMemoryPoolFinalize",                (void *)nvn_mp_finalize },
    { "nvnBufferBuilderSetDevice",            (void *)nvn_bb_set_device },
    { "nvnBufferBuilderSetDefaults",          (void *)nvn_bb_set_defaults },
    { "nvnBufferBuilderSetStorage",           (void *)nvn_bb_set_storage },
    { "nvnBufferInitialize",                  (void *)nvn_buf_init },
    { "nvnBufferGetAddress",                  (void *)nvn_buf_addr },
    { "nvnBufferGetSize",                     (void *)nvn_buf_size },
    { "nvnBufferGetMemoryOffset",             (void *)nvn_buf_offset },
    { "nvnBufferGetMemoryPool",               (void *)nvn_buf_pool },
    { "nvnBufferFlushMappedRange",            (void *)nvn_buf_flush },
    { "nvnBufferInvalidateMappedRange",       (void *)nvn_buf_inval },
    { "nvnBufferSetDebugLabel",               (void *)nvn_buf_label },
    { "nvnBufferFinalize",                    (void *)nvn_buf_finalize },
};

// ── 分派入口（探针的 nvn_real_lookup 先查这里，再查自己的表）──
typedef struct { const char *name; void *fn; } dm_nvn_ent2;
static const dm_nvn_ent2 g_lut[] = {
    { "nvnMemoryPoolBuilderSetDevice", (void *)nvn_mpb_set_device },
    { "nvnMemoryPoolBuilderSetDefaults", (void *)nvn_mpb_set_defaults },
    { "nvnMemoryPoolBuilderSetStorage", (void *)nvn_mpb_set_storage },
    { "nvnMemoryPoolBuilderSetFlags", (void *)nvn_mpb_set_flags },
    { "nvnMemoryPoolInitialize", (void *)nvn_mp_init },
    { "nvnMemoryPoolFinalize", (void *)nvn_mp_finalize },
    { "nvnMemoryPoolMap", (void *)nvn_mp_map },
    { "nvnMemoryPoolGetBufferAddress", (void *)nvn_mp_addr },
    { "nvnMemoryPoolFlushMappedRange", (void *)nvn_mp_flush },
    { "nvnMemoryPoolInvalidateMappedRange", (void *)nvn_mp_inval },
    { "nvnMemoryPoolSetDebugLabel", (void *)nvn_mp_label },
    { "nvnBufferBuilderSetDevice", (void *)nvn_bb_set_device },
    { "nvnBufferBuilderSetDefaults", (void *)nvn_bb_set_defaults },
    { "nvnBufferBuilderSetStorage", (void *)nvn_bb_set_storage },
    { "nvnBufferInitialize", (void *)nvn_buf_init },
    { "nvnBufferFinalize", (void *)nvn_buf_finalize },
    { "nvnBufferGetAddress", (void *)nvn_buf_addr },
    { "nvnBufferGetSize", (void *)nvn_buf_size },
    { "nvnBufferGetMemoryPool", (void *)nvn_buf_pool },
    { "nvnBufferGetMemoryOffset", (void *)nvn_buf_offset },
    { "nvnBufferFlushMappedRange", (void *)nvn_buf_flush },
    { "nvnBufferInvalidateMappedRange", (void *)nvn_buf_inval },
    { "nvnBufferSetDebugLabel", (void *)nvn_buf_label },
};

void *dm_nvn_vk_real(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof g_lut / sizeof g_lut[0]; i++)
        if (!strcmp(g_lut[i].name, name)) return g_lut[i].fn;
    return NULL;
}
