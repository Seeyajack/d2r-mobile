// 「刁难样本集」的公共头。
// Common header of the "hostile sample" project.
//
// 这个项目不是拿来用的，是拿来**验证 L1（垫片层 + 探针工具）真的通用**的。
// 它故意与 d2r-mobile 长得不一样：
//   · 目录是**嵌套**的（d2r-mobile 的源码是平铺的）——用来打探针的"只扫一层"假设；
//   · 只用一个 include 目录，头文件自己带前缀目录结构；
//   · 分成 src/（期望全过）与 gaps/（期望报出缺口）两半，后者是**反向验证**。
// This project is not meant to be used; it exists to verify that L1 (the shim layer plus the probe
// tools) is genuinely portable. It is deliberately unlike d2r-mobile: nested directories (d2r-mobile is
// flat), a single include root, and two halves -- src/ (expected to compile) and gaps/ (expected to
// fail), the latter serving as a negative test.
#pragma once

#include <stddef.h>
#include <stdint.h>

// ---- platform ----
uint64_t demo_mono_ns(void);
uint64_t demo_uptime_ns(void);
uint64_t demo_cpu_ns(void);

typedef struct demo_latch demo_latch;
demo_latch *demo_latch_new(void);
void        demo_latch_signal(demo_latch *l);
int         demo_latch_wait(demo_latch *l);
int         demo_latch_wait_ms(demo_latch *l, int ms);
void        demo_latch_free(demo_latch *l);
void        demo_boost(void);
int         demo_stack_top(void *p);

int      demo_peek(uint64_t addr, void *out, size_t n);
uint64_t demo_footprint(void);
uint64_t demo_malloc_bytes(void);
struct timeval;
int      demo_start_time(struct timeval *tv);

// ---- fs ----
int     demo_clone_tree(const char *src, const char *dst);
int     demo_swap_names(const char *a, const char *b);
int     demo_tag(const char *path, const char *tag);
long    demo_tag_get(const char *path, char *out, size_t n);
int     demo_untag(const char *path);
void    demo_flush_code(void *p, size_t n);
size_t  demo_copy_str(char *dst, const char *src, size_t n);
size_t  demo_append_str(char *dst, const char *src, size_t n);
uint32_t demo_rand(void);
uint32_t demo_rand_below(uint32_t n);
void    demo_fill(void *p, size_t n);
int     demo_has_marker(const char *s, size_t n);

// ---- crypto ----
void demo_md5(const void *p, size_t n, unsigned char out[16]);
void demo_sha1(const void *p, size_t n, unsigned char out[20]);
void demo_sha256(const void *p, size_t n, unsigned char out[32]);

// ---- app ----
int demo_report(char *out, size_t cap);
