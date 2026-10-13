// 单测跑测框架的实现。表由 gen_host_tests.py 生成，本文件不认识具体哪些单测。
// Implementation of the test runner. The roster is generated; this file knows no specific test.
#include "host_tests.h"

#include "dm_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// 每个单测最多给多久。测试本身是"秒级"的，10 秒还没完就当挂住了——**宁可漏报一个，也不能挂住整个 App**。
// Per-test budget. The suite is a seconds-long thing; anything still running after 10 s is treated as
// hung. Better to lose one test's result than to hang the whole app.
#define DM_TEST_TIMEOUT_MS 10000

// 失败时附上它自己打印的输出，但只附前几行——报告要塞进一个定长缓冲。
#define DM_FAIL_LINES 6
#define DM_LINE_MAX 400

typedef struct { int (*fn)(void); int rc; } job_t;

static void *job_tramp(void *p) {
    job_t *j = (job_t *)p;
    j->rc = j->fn();
    return NULL;
}

// ---- 报告：同时写进 out 缓冲、日志文件、hilog ----
typedef struct {
    char *p, *end;      // out 缓冲
    FILE *log;          // 日志文件
} sink_t;

static void emit(sink_t *s, const char *fmt, ...) {
    char line[DM_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    if (s->p < s->end) {
        int n = snprintf(s->p, (size_t)(s->end - s->p) + 1, "%s\n", line);
        if (n > 0) { s->p += n; if (s->p > s->end) s->p = s->end; }
    }
    if (s->log) { fputs(line, s->log); fputc('\n', s->log); fflush(s->log); }
    dm_log_printf("%s", line);
}

// ---- 把单测自己的 stdout 引到文件里（不引走的话，它的诊断输出在 App 里根本看不到）----
static int capture_begin(const char *path) {
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    if (saved < 0) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { close(saved); return -1; }
    dup2(fd, STDOUT_FILENO);
    close(fd);
    return saved;
}

static void capture_end(int saved) {
    fflush(stdout);
    if (saved >= 0) { dup2(saved, STDOUT_FILENO); close(saved); }
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

int dm_host_tests_run(const char *logpath, char *out, size_t cap) {
    if (!out || cap < 128) return -1;
    out[0] = 0;
    sink_t s = { out, out + cap - 2, NULL };

    if (logpath && *logpath) s.log = fopen(logpath, "w");

    emit(&s, "=== 宿主单测（上游 dm/host/tests，在鸿蒙上跑）===");
    emit(&s, "共 %d 个", dm_host_tests_count);

    char cappath[512];
    snprintf(cappath, sizeof cappath, "%s.out", logpath ? logpath : "/data/local/tmp/dmtest");

    int fails = 0, i;
    for (i = 0; i < dm_host_tests_count; i++) {
        const dm_test_ent *t = &dm_host_tests[i];
        job_t j = { t->fn, -999 };
        pthread_t th;
        uint64_t t0 = now_ms();

        int saved = capture_begin(cappath);
        int made = (pthread_create(&th, NULL, job_tramp, &j) == 0);
        if (!made) {
            capture_end(saved);
            emit(&s, "[%2d/%2d] 跳过 %-22s 起不了线程", i + 1, dm_host_tests_count, t->name);
            continue;
        }

        // 带超时地等。超时就不等了（线程留着，反正后面也不再用它）。
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        uint64_t add = (uint64_t)DM_TEST_TIMEOUT_MS * 1000000ull;
        uint64_t ns = (uint64_t)dl.tv_nsec + add;
        dl.tv_sec += (time_t)(ns / 1000000000ull);
        dl.tv_nsec = (long)(ns % 1000000000ull);

        int timed_out = 0;
        if (pthread_timedjoin_np(th, NULL, &dl) != 0) {
            timed_out = 1;
            j.rc = -998;
        }
        capture_end(saved);

        uint64_t ms = now_ms() - t0;
        int ok = (!timed_out && j.rc == 0);
        if (!ok) fails++;

        emit(&s, "[%2d/%2d] %s %-22s rc=%d  %llums",
             i + 1, dm_host_tests_count, ok ? "PASS" : "FAIL", t->name, j.rc,
             (unsigned long long)ms);

        if (!ok) {
            // 失败时把它自己打印的诊断带上来几行——不然只有一个 rc，看不出哪里错了
            FILE *f = fopen(cappath, "r");
            if (f) {
                char line[DM_LINE_MAX];
                int shown = 0;
                while (shown < DM_FAIL_LINES && fgets(line, sizeof line, f)) {
                    size_t l = strlen(line);
                    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
                    if (!l) continue;
                    emit(&s, "        %s", line);
                    shown++;
                }
                fclose(f);
            }
        }
        unlink(cappath);
    }

    emit(&s, "===== 结果：通过 %d / 失败 %d（共 %d）=====",
         dm_host_tests_count - fails, fails, dm_host_tests_count);

    if (s.log) fclose(s.log);
    return fails;
}
