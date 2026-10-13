// 报告：把前面各模块的结果拼成一行。这一份刻意**只用标准库**，用来对照
// "平台无关的部分一行都不用改"。
// Report: joins the other modules' results into one line. Deliberately standard-library only, to show
// that the platform-neutral part needs no changes at all.
#include "demoapp.h"

#include <stdio.h>
#include <string.h>

int demo_report(char *out, size_t cap) {
    unsigned char d[32];
    char tag[64] = "demo/";

    demo_sha256("demo", 4, d);
    demo_append_str(tag, "app", sizeof tag);

    int n = snprintf(out, cap, "%s mono=%llu up=%llu stack=%d sum=%02x",
                     tag,
                     (unsigned long long)demo_mono_ns(),
                     (unsigned long long)demo_uptime_ns(),
                     demo_stack_top(out),
                     d[0]);
    if (n < 0) return -1;
    return n < (int)cap ? n : (int)cap - 1;
}
