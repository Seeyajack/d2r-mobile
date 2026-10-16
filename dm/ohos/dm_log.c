// dm_log.h 的实现：一行格式化 + hilog 输出，另外镜像一份到 stderr 方便 DevEco 的调试器直接看。
// Implementation of dm_log.h: format one line, emit to hilog, and mirror to stderr for the DevEco debugger.
#include "dm_log.h"

#include <hilog/log.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// 日志域号随便取一个有辨识度的值（"DM" 的两个字节），方便 hilog 里按域过滤。
// An arbitrary but recognizable domain ("DM") so hilog output can be filtered by domain.
#define DM_LOG_DOMAIN 0x0D2Cu
#define DM_LOG_TAG    "dm"

// 上游最长的一条日志接近 1 KiB（现场导出那几行）；给 2 KiB，超出就截断并标明。
// Upstream's longest lines approach 1 KiB (the state-dump rows); 2 KiB with a truncation marker.
#define DM_LOG_LINE_MAX 2048

// 诊断镜像（探针的门回路落盘）：非 NULL 时每条 LOG 追加写一份。信号处理器里也写
// （fputs/fflush 在处理器里可接受，探针专用）。
static FILE *g_mirror;
void dm_log_set_mirror(void *f) { g_mirror = (FILE *)f; }

void dm_log_printf(const char *fmt, ...) {
    char line[DM_LOG_LINE_MAX];
    int n = snprintf(line, sizeof line, "[dm] ");

    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    if (m < 0) m = 0;
    n += m;
    if (n >= (int)sizeof line) {
        // 截断：把结尾改成省略标记，避免看起来像"日志本来就到这里结束"。
        // Truncated: mark the end so it does not look like the message just ended there.
        static const char tail[] = "...【日志过长已截断】";
        size_t t = sizeof line - sizeof tail;
        if (t < sizeof line) memcpy(line + t, tail, sizeof tail);
        n = (int)sizeof line - 1;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, DM_LOG_DOMAIN, DM_LOG_TAG, "%{public}s", line);
    // stderr 镜像：DevEco Studio 的日志窗口与 hdc 都能看到，排查时不用二选一。
    // Mirror to stderr so both the DevEco log window and hdc can see it.
    fputs(line, stderr);
    fputc('\n', stderr);
    if (g_mirror) {
        fputs(line, g_mirror);
        fputc('\n', g_mirror);
        fflush(g_mirror);
    }
}
