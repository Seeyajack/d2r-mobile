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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// 每个单测最多给多久。测试本身是"秒级"的，10 秒还没完就当挂住了——**宁可漏报一个，也不能挂住整个 App**。
#define DM_TEST_TIMEOUT_MS 10000
// 失败时附上它自己打印的输出。**屏幕报告只放前几行**（报告要塞进定长缓冲），
// 但**日志文件放全文**——查问题时读的是日志文件，截断会把答案藏起来（t_hid_script 就是这么被藏过）。
#define DM_FAIL_LINES 8
#define DM_LINE_MAX 400

// 单测的 main 是**带参数**的：上游用 `$(B)/t_i18n en`、`TMPDIR=... $(B)/t_fs_case` 这类方式跑它们。
// 不看这些直接调用 main()，轻则失败重则空指针崩溃——真机上就是这么撞出来的。
// The tests' mains take arguments: upstream runs them as `$(B)/t_i18n en` or with TMPDIR set. Calling
// main() without them fails or crashes -- which is exactly what happened on the device.
typedef int (*dm_test_fn)(int, char **);
typedef struct {
    dm_test_fn fn;
    const char *name;
    const char *arg;
    int rc;
    pthread_mutex_t m;
    pthread_cond_t c;
    int done;
} job_t;

static void *job_tramp(void *p) {
    job_t *j = (job_t *)p;
    static char a0[64], a1[64];      // argv 要活到这个调用结束；测试是串行跑的，静态够用
    snprintf(a0, sizeof a0, "%s", j->name);
    snprintf(a1, sizeof a1, "%s", j->arg);
    char *argv[3] = { a0, a1, NULL };
    int argc = (j->arg && *j->arg) ? 2 : 1;
    j->rc = j->fn(argc, argv);
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

// 只写日志文件、不进屏幕报告（失败诊断的完整内容走这里）
static void emit_log_only(sink_t *s, const char *line) {
    if (s->log) { fputs("        ", s->log); fputs(line, s->log); fputc('\n', s->log); fflush(s->log); }
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

// 按上游 recipe 给的环境跑：形如 "TMPDIR=@;DM_ROMFS=/x"，值写成 @ 表示"日志文件所在目录"。
// 上游 recipe 里这些指向构建目录；真机上换成应用自己的数据目录。
// Applies the environment from upstream's recipe; a value of @ means "the log file's directory".
static void apply_env(const char *env, const char *logdir) {
    if (!env || !*env) return;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", env);
    for (char *p = buf; p && *p; ) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = 0;
            const char *val = eq + 1;
            if (!strcmp(val, "@")) val = logdir;
            setenv(p, val, 1);
        }
        p = semi ? semi + 1 : NULL;
    }
}

// 取一个路径的目录部分（在本文件里就一处用到，不引入 libgen）
static void dir_of(const char *path, char *out, size_t cap) {
    snprintf(out, cap, "%s", path ? path : ".");
    char *slash = strrchr(out, '/');
    if (slash) *slash = 0;
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

// ---- 环境前提检查 ----
// 上游这些单测是在 macOS 上跑的，默认了几条宿主环境前提。真机上这几条**不一定成立**，
// 而一旦不成立，失败信息会指向错误的方向（比如"大小写不对"其实是 xattr 不支持）。
// 所以开跑前先把它们逐条报出来——**报告里哪几条不成立，看后面的失败就能对症**。
// The upstream tests run on macOS and assume a few host prerequisites. Those do not necessarily hold on
// the device, and when they do not, the failure points in the wrong direction. So report them up front.
static void precheck(sink_t *s, const char *logdir) {
    emit(s, "---- 环境前提检查 ----");

    // 1) /tmp 存不存在（t_fs 硬编码了 /tmp）
    struct stat st;
    int has_tmp = (stat("/tmp", &st) == 0);
    emit(s, "  /tmp 存在：%s%s", has_tmp ? "是" : "**否**",
         has_tmp ? "" : "   ← t_fs 硬编码 /tmp，会因此早退");

    // 2) 数据目录可写
    emit(s, "  数据目录可写：%s", (access(logdir, W_OK) == 0) ? "是" : "**否**");

    // 3) xattr 能不能用（hle_fs 的路由全靠它：宿主文件上贴"客体原名"）
    //    直接用系统调用，免得跟垫片头混在一起看不清是谁的问题
    char f[1200];
    snprintf(f, sizeof f, "%s/.xattr-probe", logdir);
    FILE *fp = fopen(f, "w");
    int sx = -1, gx = -1, sx_errno = 0, gx_errno = 0;
    if (fp) {
        fclose(fp);
        errno = 0;
        sx = (int)syscall(SYS_setxattr, f, "user.dmprobe", "1", (size_t)1, 0);
        sx_errno = errno;
        errno = 0;
        char v[8];
        gx = (int)syscall(SYS_getxattr, f, "user.dmprobe", v, sizeof v);
        gx_errno = errno;
        unlink(f);
    }
    emit(s, "  xattr 可用：setxattr rc=%d(errno=%d) getxattr rc=%d(errno=%d) %s",
         sx, sx_errno, gx, gx_errno,
         (gx >= 0 || gx_errno == ENODATA) ? "→ 可用"
                                          : "→ **不可用**：hle_fs 的路径查找会全线失败，"
                                            "后面 t_fs_case 的失败应当归因到这里");

    // 4) fork 能不能用（t_hid_script 有两条检查靠 fork + 等子进程被信号杀掉）
    errno = 0;
    pid_t pid = fork();
    if (pid < 0) {
        emit(s, "  fork 可用：**否**（errno=%d）   ← t_hid_script 里靠 fork 的检查必然失败", errno);
    } else if (pid == 0) {
        _exit(0);                                  // 子进程：立刻退出
    } else {
        int status = 0;
        waitpid(pid, &status, 0);
        emit(s, "  fork 可用：是（子进程正常退出）");
    }
    emit(s, "");
}

int dm_host_tests_run(const char *logpath, const char *filter, char *out, size_t cap) {
    if (!out || cap < 128) return -1;
    out[0] = 0;
    sink_t s = { out, out + cap - 2, NULL };

    if (!filter) filter = "";
    int only_named = (*filter && strcmp(filter, "*") != 0);   // 只跑指定那一个
    int fresh = (strcmp(filter, "*") == 0);                   // 清空日志重头跑

    snprintf(g_logpath, sizeof g_logpath, "%s", (logpath && *logpath) ? logpath : "/data/local/tmp/dmtest");
    if (fresh) unlink(g_logpath);

    // 续跑：先看日志里已经有哪些结果。**打开是追加模式**，这样之前的结果不会被这次覆盖掉。
    char *started = (char *)calloc((size_t)dm_host_tests_count, 1);
    char *finished = (char *)calloc((size_t)dm_host_tests_count, 1);
    if (!only_named && started && finished) load_done(g_logpath, started, finished, dm_host_tests_count);
    s.log = fopen(g_logpath, "a");

    arm_fatal_handlers();

    char dir[1024] = { 0 };
    int have_dir = (self_dir(dir, sizeof dir) == 0);
    char logdir[1024] = { 0 };
    dir_of(g_logpath, logdir, sizeof logdir);

    emit(&s, "=== 宿主单测（上游 dm/host/tests，在鸿蒙上跑）===");
    emit(&s, "共 %d 条；自身目录：%s；筛选：%s；数据目录：%s", dm_host_tests_count,
         have_dir && *dir ? dir : "（取不到）", *filter ? filter : "（续跑）", logdir);
    precheck(&s, logdir);

    char cappath[512];
    snprintf(cappath, sizeof cappath, "%s.out", g_logpath);

    int fails = 0, loaded_fail = 0, i, ran = 0;
    for (i = 0; i < dm_host_tests_count; i++) {
        const dm_test_ent *t = &dm_host_tests[i];
        int was_finished = finished ? finished[i] : 0;
        int was_started = started ? started[i] : 0;
        g_cur_name = t->name;

        // 只跑指定的那一个：其余一概跳过
        if (only_named && strcmp(t->name, filter) != 0) continue;

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
        j.fn = fn;
        j.name = t->name;
        j.arg = t->arg ? t->arg : "";
        j.rc = -999;
        pthread_mutex_init(&j.m, NULL);
        pthread_cond_init(&j.c, NULL);

        // 按上游 recipe 给的环境跑（TMPDIR / DM_ROMFS 之类）
        apply_env(t->env, logdir);

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
                int shown = 0, total = 0;
                while (fgets(line, sizeof line, f)) {
                    size_t l = strlen(line);
                    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
                    if (!l) continue;
                    total++;
                    if (shown < DM_FAIL_LINES) { emit(&s, "        %s", line); shown++; }
                    else emit_log_only(&s, line);   // 屏幕放不下，日志里放全
                }
                if (total > shown) emit(&s, "        （共 %d 行诊断，其余写在日志文件里）", total);
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
