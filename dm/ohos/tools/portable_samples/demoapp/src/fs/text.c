// 字符串与随机数：BSD 的 strlcpy/strlcat、arc4random 系列、GNU 的 memmem。
// Strings and randomness: BSD strlcpy/strlcat, the arc4random family, and GNU memmem.
//
// 这些都**不需要头文件**（在苹果上由 <string.h> / <stdlib.h> 顺带声明），
// 所以按头文件数根本数不到——垫片层靠**逐符号 grep** 才找出来。
// None of these need a header of their own (Apple declares them through <string.h> / <stdlib.h>), so a
// header census cannot find them; only a symbol-level grep does.
#include "demoapp.h"

#include <stdlib.h>
#include <string.h>

size_t demo_copy_str(char *dst, const char *src, size_t n) { return strlcpy(dst, src, n); }

size_t demo_append_str(char *dst, const char *src, size_t n) { return strlcat(dst, src, n); }

uint32_t demo_rand(void) { return arc4random(); }

uint32_t demo_rand_below(uint32_t n) { return arc4random_uniform(n); }

void demo_fill(void *p, size_t n) { arc4random_buf(p, n); }

int demo_has_marker(const char *s, size_t n) {
    // memmem 在 musl 里要 _GNU_SOURCE 才声明；不声明也能编过，但返回值会被当成 int
    return memmem(s, n, "\xee\x81\xbe", 3) != NULL;
}
