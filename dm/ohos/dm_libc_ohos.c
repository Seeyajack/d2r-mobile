// 通用 libc 胶水层（L1）—— 见 dm_libc_ohos.h 顶部注释（为什么必须有这一层）。
// General libc glue (L1) — see the header for why this layer has to exist.
//
// 设计：只做"符号名 → 宿主函数指针"的直连，**不经过记录型存根**。
// 理由：libc 是高频调用（memcpy 一轮 279 次、memset 318 次），走桩等于每帧多两次跳转 + 日志，
// 且桩的语义是"返回 0"，对 memcpy/strcmp 这类函数是**错误语义**而不是"暂时缺失"。
// Design: a plain name -> host-function-pointer table, with no recording stub in between.
// These are hot and their stubbed semantics (return 0) are *wrong*, not merely missing.

#include "dm_libc_ohos.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

static void (*g_log)(const char *, ...);
void dm_libc_set_log(void (*fn)(const char *, ...)) { g_log = fn; }

// ── 需要包一层的少数几个 ────────────────────────────────────────────────
// musl 的 memalign(alignment, size)（参数顺序与 aligned_alloc 相反）。
static void *dm_memalign(size_t align, size_t size) {
    void *p = NULL;
    if (align < sizeof(void *)) align = sizeof(void *);
    if (posix_memalign(&p, align, size) != 0) return NULL;
    return p;
}
static int *dm_errno_location(void) { return &errno; }

// nnSdk 的 musl 初始化钩子：客体里由加载器调用，宿主没有对应阶段 ⇒ 空实现（返回 0 无害）。
static int dm_nop0(void) { return 0; }
static int dm_nop1(uint64_t a) { (void)a; return 0; }
static int dm_nop2(uint64_t a, uint64_t b) { (void)a; (void)b; return 0; }

// ── 符号表 ──────────────────────────────────────────────────────────────
// 名字必须与客体 .dynstr 里**逐字节一致**（C 符号，无 Itanium 修饰）。
#define E(f) { #f, (void *)(uintptr_t)(f) }
#define EA(sym, f) { sym, (void *)(uintptr_t)(f) }

static const struct { const char *name; void *fn; } g_libc[] = {
    // 内存（最关键：memcpy 为空操作 = 所有结构体拷贝丢失）
    E(memcpy), E(memmove), E(memset), E(memcmp),
    EA("memalign", dm_memalign),
    // 字符串（strcmp 恒 0 = 所有"按名字查找"假成功）
    E(strlen), E(strcpy), E(strncpy), E(strcmp), E(strncmp), E(strchr), E(strstr),
    E(strnlen), E(strncat), E(strncasecmp),
    // 数值/字符转换
    E(strtod), E(strtoul), E(tolower), E(sscanf), E(vsnprintf),
    // 数学（单精度为主：客体是 32 位浮点管线）
    E(sinf), E(cosf), E(tanf), E(asinf), E(acosf), E(atanf), E(atan2f),
    E(sqrtf), E(powf), E(expf), E(exp2f), E(logf), E(log10f), E(fabsf), E(fmodf),
    E(sin), E(sqrt), E(pow),
    // 时间
    E(time), E(clock),
    // 杂项
    EA("__errno_location", dm_errno_location),
    EA("_init_libc0", dm_nop0), EA("_init_libc1", dm_nop1), EA("_init_libc2", dm_nop2),
    EA("__nnmusl_init_dso", dm_nop2), EA("__nnmusl_fini_dso", dm_nop2),
};

void *dm_libc_impl(const char *sym_name) {
    if (!sym_name || !*sym_name) return NULL;
    for (size_t i = 0; i < sizeof g_libc / sizeof g_libc[0]; i++)
        if (!strcmp(sym_name, g_libc[i].name)) return g_libc[i].fn;
    return NULL;
}
