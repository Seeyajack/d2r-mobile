// 单测跑测框架的实现：按名字 dlopen 每个单测 .so，取它的 main 来调。
// Implementation of the runner: dlopen each test .so by name and call its main.
//
// 表由 gen_host_tests.py 生成，本文件不认识具体哪些单测。
// The roster is generated; this file knows no specific test.
//
// ⚠️ 核心约束（真机上撞出来的）：**有些单测会让整个进程死掉**（它们用 assert，assert 触发就是 abort；
// 进程一死，后面 20 多个单测就再也跑不到了）。App 里既不能 fork 也不能 exec，没法把它们隔离到子进程。
// 所以这一版改成**可续跑**：
//   · 每跑完一个就把机器可读的结果行 `@RESULT <名字> PASS|FAIL` 写进日志；
//   · 启动时先读日志，已经出过结果的单测**跳过**；
//   · 给 SIGABRT / SIGSEGV / SIGBUS 装处理函数，只做异步信号安全的事（write 一行 `@CRASH <名字> <信号>`）
//     然后 _exit——这样下一轮就知道是崩在谁身上，并且跳过它继续往后跑。
// 于是"点一次按钮"推进一段，崩了就再点一次，直到 26 个都跑过一遍。
// ⚠️ The hard constraint measured on the device: some tests kill the whole process (they use assert, and
// assert aborts; once the process dies the remaining twenty-odd tests can never run). An app can neither
// fork nor exec, so tests cannot be isolated into child processes. Hence this version is resumable:
// each finished test writes a machine-readable result line; on start the log is read and already-finished
// tests are skipped; SIGABRT/SIGSEGV/SIGBUS handlers write a single async-signal-safe crash line and
// _exit, so the next round knows who crashed and moves past it.
#include "host_tests.h"

#include "dm_log.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// 每个单测最多给多久。测试本身是"秒级"的，10 秒还没完就当挂住了——**宁可漏报一个，也不能挂住整个 App**。
#define DM_TEST_TIMEOUT_MS 10000
#define DM_FAIL_LINES 6
#define DM_LINE_MAX 400

typedef int (*dm_test_fn)(void);
typedef struct {
    dm_test_fn fn;
    int rc;
    pthread_mutex_t m;
    pthread_cond_t c;
    int done;
} job_t;

static void *job_tramp(void *p) {
    job_t *j = (job_t *)p;
    j->rc = j->fn();
    pthread_mutex_lock(&j->m);
    j->done = 1;
    pthread_cond_signal(&j->c);
    pthread_mutex_unlock(&j->m);
    return NULL;
}

// ---- 报告：同时写进 out 缓冲、日志文件、hilog ----
typedef struct { char *p, *end; FILE *log; } sink_t;

static char g_logpath[1024];
static const char *volatile g_cur_name;      // 当前正在跑的单测名，崩了要靠它认人

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

// ---- 崩了怎么办：只做异步信号安全的事（open/write/close），写一行谁崩的，然后退出 ----
static void wstr(int fd, const char *s) { if (s) { size_t n = strlen(s); if (n) { ssize_t r = write(fd, s, n); (void)r; } } }

static void on_fatal(int sig) {
    int fd = open(g_logpath, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        wstr(fd, "@CRASH ");
        wstr(fd, g_cur_name ? g_cur_name : "?");
        wstr(fd, sig == SIGABRT ? " SIGABRT\n" : sig == SIGSEGV ? " SIGSEGV\n" :
                 sig == SIGBUS ? " SIGBUS\n" : " SIG\n");
        close(fd);
    }
    _exit(3);
}

static void arm_fatal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;              // 处理函数自己再出事就别再进来
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

// ---- 续跑：先读日志，出过结果（或上次跑死在这里）的单测就不再跑 ----
// 三种标记：
//   @BEGIN  <名字>   —— 开跑前写。**只要它没有配对的 @RESULT，就说明这个单测上次把进程搞死了**。
//   @RESULT <名字> PASS|FAIL|…  —— 跑完了。
//   @CRASH  <名字> <信号>       —— 被信号打断（abort 之类）；`exit()` 的不走这条，只能靠 @BEGIN 认出来。
// 为什么要有 @BEGIN：t_condvar 这类单测直接调 exit()，进程干净退出，信号处理函数根本不会触发，
// 光靠 @RESULT / @CRASH 无法知道是谁死的。
static void load_done(const char *path, char *started, char *finished, int n) {
    memset(started, 0, (size_t)n);
    memset(finished, 0, (size_t)n);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[DM_LINE_MAX];
    while (fgets(line, sizeof line, f)) {
        const char *tag = NULL;
        int fin = 0;
        if (!strncmp(line, "@BEGIN ", 7)) { tag = line + 7; }
        else if (!strncmp(line, "@RESULT ", 8)) { tag = line + 8; fin = 1; }
        else if (!strncmp(line, "@CRASH ", 7)) { tag = line + 7; fin = 1; }
        if (!tag) continue;
        for (int i = 0; i < dm_host_tests_count; i++) {
            size_t l = strlen(dm_host_tests[i].name);
            if (!strncmp(tag, dm_host_tests[i].name, l) && (tag[l] == ' ' || tag[l] == '\n')) {
                started[i] = 1;
                if (fin) finished[i] = 1;
                break;
            }
        }
    }
    fclose(f);
}

// ---- 找到自己所在目录：单测 .so 就在同一个目录里，这样不用让 ArkTS 传路径过来 ----
static int self_dir(char *out, size_t cap) {
    Dl_info info;
    if (!dladdr((void *)&dm_host_tests_run, &info) || !info.dli_fname) return -1;
    const char *fn = info.dli_fname;
    const char *slash = strrchr(fn, '/');
    if (!slash) { out[0] = 0; return 0; }
    size_t n = (size_t)(slash - fn);
    if (n + 1 > cap) return -1;
    memcpy(out, fn, n);
    out[n] = 0;
    return 0;
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

static void *open_test(const char *dir, const char *lib, char *tried, size_t tcap) {
    char path[1024];
    if (dir && *dir) {
        snprintf(path, sizeof path, "%s/%s.so", dir, lib);
        snprintf(tried, tcap, "%s", path);
        void *h = dlopen(path, RTLD_NOW);
        if (h) return h;
        snprintf(tried, tcap, "%s（失败：%s）", path, dlerror());
    }
    snprintf(path, sizeof path, "%s.so", lib);
    void *h = dlopen(path, RTLD_NOW);
    if (!h) snprintf(tried, tcap, "%s（失败：%s）", path, dlerror());
    else snprintf(tried, tcap, "%s", path);
    return h;
}

int dm_host_tests_run(const char *logpath, char *out, size_t cap) {
    if (!out || cap < 128) return -1;
    out[0] = 0;
    sink_t s = { out, out + cap - 2, NULL };

    snprintf(g_logpath, sizeof g_logpath, "%s", (logpath && *logpath) ? logpath : "/data/local/tmp/dmtest");

    // 续跑：先看日志里已经有哪些结果。**打开是追加模式**，这样之前的结果不会被这次覆盖掉。
    char *started = (char *)calloc((size_t)dm_host_tests_count, 1);
    char *finished = (char *)calloc((size_t)dm_host_tests_count, 1);
    if (started && finished) load_done(g_logpath, started, finished, dm_host_tests_count);
    s.log = fopen(g_logpath, "a");

    arm_fatal_handlers();

    char dir[1024] = { 0 };
    int have_dir = (self_dir(dir, sizeof dir) == 0);

    emit(&s, "=== 宿主单测（上游 dm/host/tests，在鸿蒙上跑）===");
    emit(&s, "共 %d 个；自身目录：%s", dm_host_tests_count, have_dir && *dir ? dir : "（取不到）");

    char cappath[512];
    snprintf(cappath, sizeof cappath, "%s.out", g_logpath);

    int fails = 0, loaded_fail = 0, i, ran = 0;
    for (i = 0; i < dm_host_tests_count; i++) {
        const dm_test_ent *t = &dm_host_tests[i];
        int was_finished = finished ? finished[i] : 0;
        int was_started = started ? started[i] : 0;
        g_cur_name = t->name;

        if (was_finished) {
            emit(&s, "[%2d/%2d] 跳过 %-20s （日志里已有结果）", i + 1, dm_host_tests_count, t->name);
            continue;
        }
        if (was_started) {
            // 上次有 @BEGIN 却没有 @RESULT ⇒ 就是这个单测把进程搞死的（不管它用的是 abort 还是 exit）
            emit(&s, "[%2d/%2d] 跳过 %-20s ⚠️ 上次跑死在这里，这次不重跑", i + 1, dm_host_tests_count, t->name);
            if (s.log) { fprintf(s.log, "@RESULT %s DIED\n", t->name); fflush(s.log); }
            continue;
        }

        // **开跑前先记一笔**：进程怎么死都能靠它认出是谁
        if (s.log) { fprintf(s.log, "@BEGIN %s\n", t->name); fflush(s.log); }

        char tried[1024];
        void *h = open_test(dir, t->lib, tried, sizeof tried);
        if (!h) {
            loaded_fail++; fails++;
            emit(&s, "[%2d/%2d] 载入失败 %-18s %s", i + 1, dm_host_tests_count, t->name, tried);
            if (s.log) { fprintf(s.log, "@RESULT %s LOADFAIL\n", t->name); fflush(s.log); }
            continue;
        }
        dm_test_fn fn = (dm_test_fn)dlsym(h, "main");
        if (!fn) {
            loaded_fail++; fails++;
            emit(&s, "[%2d/%2d] 找不到入口 %-16s dlsym(main)：%s", i + 1, dm_host_tests_count, t->name, dlerror());
            if (s.log) { fprintf(s.log, "@RESULT %s NOENTRY\n", t->name); fflush(s.log); }
            dlclose(h);
            continue;
        }

        job_t j;
        memset(&j, 0, sizeof j);
        j.fn = fn; j.rc = -999;
        pthread_mutex_init(&j.m, NULL);
        pthread_cond_init(&j.c, NULL);

        pthread_t th;
        uint64_t t0 = now_ms();
        int saved = capture_begin(cappath);
        int made = (pthread_create(&th, NULL, job_tramp, &j) == 0);
        if (!made) {
            capture_end(saved);
            emit(&s, "[%2d/%2d] 起不了线程 %-18s", i + 1, dm_host_tests_count, t->name);
            dlclose(h); fails++;
            continue;
        }

        // 带超时地等：**只用纯 POSIX 的条件变量定时等待**，不用 pthread_timedjoin_np
        // （musl 没有这个 GNU 扩展，用了会在链接时炸）。
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        uint64_t ns = (uint64_t)dl.tv_nsec + (uint64_t)DM_TEST_TIMEOUT_MS * 1000000ull;
        dl.tv_sec += (time_t)(ns / 1000000000ull);
        dl.tv_nsec = (long)(ns % 1000000000ull);

        pthread_mutex_lock(&j.m);
        while (!j.done) if (pthread_cond_timedwait(&j.c, &j.m, &dl) == ETIMEDOUT && !j.done) break;
        int timed_out = !j.done;
        pthread_mutex_unlock(&j.m);
        if (timed_out) j.rc = -998; else pthread_join(th, NULL);
        capture_end(saved);
        ran++;

        uint64_t ms = now_ms() - t0;
        int ok = (!timed_out && j.rc == 0);
        if (!ok) fails++;

        emit(&s, "[%2d/%2d] %s %-20s rc=%d  %llums",
             i + 1, dm_host_tests_count, ok ? "PASS" : "FAIL", t->name, j.rc, (unsigned long long)ms);
        if (s.log) { fprintf(s.log, "@RESULT %s %s\n", t->name, ok ? "PASS" : "FAIL"); fflush(s.log); }

        if (!ok) {
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
            if (timed_out) emit(&s, "        （超过 %d ms 未结束，已放弃等待）", DM_TEST_TIMEOUT_MS);
        }
        unlink(cappath);

        // 超时的情况**不要 dlclose**：那个线程还在里面跑，卸载会把它的代码抽走 ⇒ 直接崩。
        if (!timed_out) dlclose(h);
    }

    emit(&s, "===== 本轮：新跑 %d 个，失败 %d 个（含载入失败 %d）／共 %d 个单测 =====",
         ran, fails, loaded_fail, dm_host_tests_count);
    emit(&s, "说明：某个单测若让进程死掉，日志里会留一行 @CRASH <名字>；**再点一次按钮会跳过它继续往后跑**。");
    emit(&s, "日志文件：%s", g_logpath);

    if (s.log) fclose(s.log);
    free(started);
    free(finished);
    return fails;
}
