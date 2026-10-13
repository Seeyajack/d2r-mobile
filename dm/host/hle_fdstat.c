// 诊断 DM_FD_LOG=<秒>：每隔这么多秒记一行「进程在用的文件描述符数与构成」（长时间游玩会不会撞打开文件数上限：iOS 软上限缺省 256，main.m 启动时已提到 4096）。
// 逐个描述符 fstat 判类型；普通文件 / 目录用 fcntl(F_GETPATH) 取路径后按去处归类。只读、不动任何描述符；不设变量时不起线程。
// Diagnostic DM_FD_LOG=<seconds>: every this many seconds log one line with "the number and breakdown of file descriptors the process has in use" (whether long play sessions will hit the open file limit: the iOS soft limit defaults to 256, already raised to 4096 at startup in main.m).
// fstat each descriptor to determine its type; for regular files / directories get the path with fcntl(F_GETPATH) and classify by where it points. Read-only, touches no descriptor; no thread is started when the variable is unset.
#include "hle.h"
#include "dm.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

// 取"这个描述符指向哪个路径"：
//   苹果 fcntl(fd, F_GETPATH, buf) —— fcntl 的扩展，Linux 没有；
//   鸿蒙(Linux) 的等价物是读 /proc/self/fd/<n> 这个符号链接（读到的就是路径）。
// 两者语义一致、都成功返回 0。这里收口一次，调用处写法不变。
// Getting "which path does this descriptor point to": Apple uses fcntl(fd, F_GETPATH, buf), an fcntl
// extension Linux lacks; the equivalent on HarmonyOS (Linux) is to read the /proc/self/fd/<n> symlink.
// Semantics match and both return 0 on success. Funnelled through one place so call sites are unchanged.
#if defined(__OHOS__)
static int dm_fd_path(int fd, char *out, size_t n) {
    char link[64];
    if (snprintf(link, sizeof link, "/proc/self/fd/%d", fd) >= (int)sizeof link) return -1;
    ssize_t r = readlink(link, out, n - 1);
    if (r <= 0) return -1;                       // 打不开、或指向已删除文件（readlink 返回空串）都算取不到
    out[r] = '\0';
    return 0;
}
#define DM_FD_PATH(fd, buf) dm_fd_path((fd), (buf), sizeof (buf))
#else
#define DM_FD_PATH(fd, buf) fcntl((fd), F_GETPATH, (buf))
#endif

static void *fdstat_main(void *arg) {
    int every = (int)(intptr_t)arg; char prev[1024] = "";
    for (unsigned round = 1;; round++) {
        sleep((unsigned)every);
        struct rlimit rl = {0}; getrlimit(RLIMIT_NOFILE, &rl); int max = getdtablesize(); if (max > 8192) max = 8192;
        int total = 0, romfs = 0, save = 0, logf = 0, cache = 0, dev = 0, sock = 0, pipe_ = 0, kq = 0, dir = 0, other = 0, hi = -1; char sample[400] = ""; int sk = 0;
        for (int fd = 0; fd < max; fd++) { struct stat st; if (fstat(fd, &st)) continue; total++; hi = fd;
            if (S_ISSOCK(st.st_mode)) { sock++; continue; } if (S_ISFIFO(st.st_mode)) { pipe_++; continue; } if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) { dev++; continue; }
            char p[MAXPATHLEN] = ""; if (DM_FD_PATH(fd, p) || !p[0]) { if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) other++; else kq++; continue; }
            if (S_ISDIR(st.st_mode)) dir++;
            if (strstr(p, "romfs") || strstr(p, "RomFS") || strstr(p, "/data/hd/") || strstr(p, ".casc") || strstr(p, "/Data/data/")) romfs++;
            else if (strstr(p, "/save") || strstr(p, ".d2s") || strstr(p, ".d2i") || strstr(p, "/user/")) save++;
            else if (strstr(p, ".log") || strstr(p, "boot.txt") || strstr(p, ".jsonl") || strstr(p, ".tsv")) logf++;
            else if (strstr(p, "/Caches/") || strstr(p, "/cache/") || strstr(p, ".metallib") || strstr(p, "/tmp/") || strstr(p, "/var/folders/")) cache++;
            else { other++; if (sk < 300) { const char *b = strrchr(p, '/'); sk += snprintf(sample + sk, sizeof sample - sk, " %s", b ? b + 1 : p); } } }
        char line[1024]; snprintf(line, sizeof line, "在用 %d（最大号 %d，软上限 %llu）：资源档 %d、存档 %d、日志 %d、缓存 / 临时 %d、设备 %d、套接字 %d、管道 %d、其它内核对象 %d、其它文件 %d%s%s；其中目录 %d",
                                   total, hi, (unsigned long long)rl.rlim_cur, romfs, save, logf, cache, dev, sock, pipe_, kq, other, other ? "：" : "", other ? sample : "", dir);
        LOG("描述符 第 %u 次（每 %d 秒）%s%s", round, every, line, strcmp(line, prev) ? "" : "（同上次）"); strlcpy(prev, line, sizeof prev);
    }
    return NULL;
}
__attribute__((constructor)) static void fdstat_init(void) {
    const char *e = getenv("DM_FD_LOG"); int every = e ? atoi(e) : 0; if (every <= 0) return;
    pthread_t t; if (!pthread_create(&t, NULL, fdstat_main, (void *)(intptr_t)every)) pthread_detach(t);
}
