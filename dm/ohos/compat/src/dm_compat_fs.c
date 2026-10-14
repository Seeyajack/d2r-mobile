// 垫片实现：文件与内存统计相关的四件东西——sysctl / clonefile / xattr / malloc 统计。
// Shim implementation: four file- and memory-related pieces -- sysctl / clonefile / xattr / malloc stats.
#include "sys/sysctl.h"
#include "sys/clonefile.h"
#include "sys/xattr.h"
#include "malloc/malloc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

// ================================================================ sysctl（只做 KERN_PROC_PID）

// 宿主用它取"进程创建时刻"（main.c 的 proc_start_mono_ns），用来把启动时间线的零点对齐到进程启动。
// 苹果直接给 p_starttime；Linux/鸿蒙没有这个接口，但可以算：
//     进程已运行的秒数 = /proc/self/stat 第 22 字段（自启动起的时钟滴答） / 每秒滴答数
//     进程创建时刻（墙钟） = 现在（墙钟） − 进程已运行的秒数
// 算不出来（读不到文件、字段不对）就返回 -1，调用方本来就写了回退，不会崩。
// The host uses this for the process creation time (proc_start_mono_ns in main.c) to align the boot
// timeline. Linux/OHOS has no such interface, but it can be derived from field 22 of /proc/self/stat
// (clock ticks since boot) divided by ticks-per-second, subtracted from the current wall clock. If it
// cannot be derived, return -1; the caller already has a fallback.
static int dm_proc_start_timeval(struct timeval *out) {
    char buf[1024];
    int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t r = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (r <= 0) return -1;
    buf[r] = '\0';

    // 第 2 字段是 (comm)，里面可能有空格与括号：先找到最后一个 ')'，后面的字段才按空格切。
    // Field 2 is (comm), which may contain spaces and parentheses, so start after the last ')'.
    char *p = strrchr(buf, ')');
    if (!p || p[1] != ' ') return -1;
    p += 2;
    char *tok[64]; int nt = 0, pos = 3; char *save = NULL;
    for (char *t = strtok_r(p, " \n", &save); t && nt < 64; t = strtok_r(NULL, " \n", &save), pos++) tok[nt++] = t;
    if (pos - 1 < 22 || nt < 20) return -1;                 // 确保第 22 字段（下标 19）拿到了
    long long ticks = strtoll(tok[19], NULL, 10);

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    long long up_ticks = 0;
    FILE *up = fopen("/proc/uptime", "r");
    if (!up) return -1;
    double up_sec = 0;
    if (fscanf(up, "%lf", &up_sec) != 1) { fclose(up); return -1; }
    fclose(up);
    up_ticks = (long long)(up_sec * hz);

    struct timeval now;
    if (gettimeofday(&now, NULL)) return -1;
    long long age_us = (up_ticks - ticks) * 1000000ll / hz;   // 进程已运行多少微秒
    long long now_us = (long long)now.tv_sec * 1000000ll + now.tv_usec;
    long long start_us = now_us - age_us;
    out->tv_sec = (time_t)(start_us / 1000000ll);
    out->tv_usec = (suseconds_t)(start_us % 1000000ll);
    return 0;
}

int sysctl(const int *name, unsigned int namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen) {
    (void)newp; (void)newlen;
    if (!name || namelen < 2) { errno = EINVAL; return -1; }
    if (name[0] == CTL_KERN && name[1] == KERN_PROC) {
        if (!oldp || !oldlenp) { errno = EINVAL; return -1; }
        if (namelen < 4 || name[2] != KERN_PROC_PID) { errno = ENOTSUP; return -1; }
        // 只支持查自己（宿主传的就是 getpid()）；查别人直接不支持，如实返回失败。
        // Only "self" is supported (the host passes getpid()); querying others fails honestly.
        if ((pid_t)name[3] != getpid()) { errno = ENOTSUP; return -1; }
        if (*oldlenp < sizeof(struct kinfo_proc)) { errno = ENOMEM; return -1; }
        struct kinfo_proc *kp = (struct kinfo_proc *)oldp;
        memset(kp, 0, sizeof *kp);
        if (dm_proc_start_timeval(&kp->kp_proc.p_starttime)) { errno = ENOTSUP; return -1; }
        *oldlenp = sizeof *kp;
        return 0;
    }
    errno = ENOTSUP;
    return -1;
}

// ================================================================ clonefile（递归拷贝）

// 苹果的 clonefile 在一个"写时复制文件系统"上近乎零成本。鸿蒙上没有这条捷径，
// 于是用**递归拷贝**给出功能等价的结果（克隆出来的两份互相独立，与 APFS 克隆后的语义一致），
// 代价是真实的数据拷贝。存档目录是几 MB 量级，这个代价可以接受。
// Apple's clonefile is nearly free on a copy-on-write filesystem. HarmonyOS has no such shortcut, so a
// recursive copy gives the functionally equivalent result (the two trees become independent) at the cost
// of real data movement. Save directories are a few MB, so this is acceptable.
static int dm_copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st)) return -1;

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 07777) && errno != EEXIST) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        int rc = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char a[4096], b[4096];
            if (snprintf(a, sizeof a, "%s/%s", src, e->d_name) >= (int)sizeof a ||
                snprintf(b, sizeof b, "%s/%s", dst, e->d_name) >= (int)sizeof b) { rc = -1; break; }
            if (dm_copy_tree(a, b)) { rc = -1; break; }
        }
        closedir(d);
        return rc;
    }

    if (S_ISLNK(st.st_mode)) {
        char target[4096];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = '\0';
        unlink(dst);
        return symlink(target, dst);
    }

    if (!S_ISREG(st.st_mode)) {
        // 设备节点 / 套接字 / FIFO：存档目录里不该出现，出现了就明确报错，不假装成功。
        // Device nodes, sockets and FIFOs should not appear in a save directory; error out rather than pretend.
        errno = ENOTSUP;
        return -1;
    }

    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    int rc = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof buf);
        if (r == 0) break;
        if (r < 0) { if (errno == EINTR) continue; rc = -1; break; }
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            off += w;
        }
        if (rc) break;
    }
    close(in);
    if (close(out)) rc = -1;
    if (rc) unlink(dst);
    return rc;
}

int clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    if (!src || !dst) { errno = EINVAL; return -1; }
    if (dm_copy_tree(src, dst)) return -1;
    return 0;
}

int clonefileat(int src_dirfd, const char *src, int dst_dirfd, const char *dst, int flags) {
    // 宿主用的是 clonefile（绝对路径），这条只是为了对齐头文件签名。
    // The host uses clonefile with absolute paths; this exists only to match the header signature.
    if (src_dirfd != AT_FDCWD || dst_dirfd != AT_FDCWD) { errno = ENOTSUP; return -1; }
    return clonefile(src, dst, flags);
}

// ================================================================ xattr（Darwin 6 参数 → Linux 5/4 参数）
//
// 宿主只在一处用它：给宿主文件贴一个"客体原名"的属性，解决"APFS 不分大小写 vs Horizon 分大小写"。
// 位置参数（position）苹果也只支持 0，这里同样只支持 0。
// Used in one place: tagging a host file with the guest's original name. Apple supports only position 0
// as well, and so does this.
// 真机实测出来的一处平台差异：**查一个不存在的属性**时，鸿蒙给的是 EOPNOTSUPP(95)，
// 而不是 Linux 常规的 ENODATA(61)（= 苹果的 ENOATTR）。
// 这一处小差异的后果很大：hle_fs 的 guest_name() 是"拿到 ENOATTR → 用物理名，拿到别的 → 整条路径判失败"，
// 于是真机上**每一次路径解析都失败**，t_fs_case 的 51 项里 44 项一起挂。
//
// 为什么把它归一化成 ENODATA 是安全的：读到"没有这个名字"之后，hle_fs 的既定行为就是
// **回退成"物理名即客体名"**——而万一某个文件系统真的完全不支持 xattr，这个回退正是我们想要的，
// 不会比"整条路径失败"更糟。
// Measured on the device: reading a **non-existent** attribute returns EOPNOTSUPP(95) rather than the
// usual ENODATA(61) (= Apple's ENOATTR). The consequence is large: hle_fs's guest_name() treats
// "ENOATTR" as "use the physical name" and anything else as a hard path failure, so on the device every
// path resolution failed and 44 of t_fs_case's 51 checks went down with it.
// Normalising to ENODATA is safe: the caller's established fallback is "physical name == guest name",
// which is exactly what we would want even if a filesystem supported no xattrs at all.
// 真机实测出来的一处平台差异：**查一个不存在的属性**时，鸿蒙给的是 EOPNOTSUPP(95)，
// 而不是 Linux 常规的 ENODATA(61)（= 苹果的 ENOATTR）。
// 这一处小差异的后果很大：hle_fs 的 guest_name() 是"拿到 ENOATTR → 用物理名，拿到别的 → 整条路径判失败"，
// 于是真机上**每一次路径解析都失败**，t_fs_case 的 51 项里 44 项一起挂。
//
// 为什么把它归一化成 ENODATA 是安全的：读到"没有这个名字"之后，hle_fs 的既定行为就是
// **回退成"物理名即客体名"**——而万一某个文件系统真的完全不支持 xattr，这个回退正是我们想要的，
// 不会比"整条路径失败"更糟。
// Measured on the device: reading a **non-existent** attribute returns EOPNOTSUPP(95) rather than the
// usual ENODATA(61) (= Apple's ENOATTR). hle_fs's guest_name() treats "ENOATTR" as "use the physical
// name" and anything else as a hard path failure, so on the device every path resolution failed.
// Normalising to ENODATA is safe: the caller's established fallback is "physical name == guest name".
// Linux 的 xattr 名**必须带命名空间前缀**（user. / security. / system. / trusted.）；
// 苹果允许任意名字（上游用的是 org.d2rmobile.guest-name，没有前缀）。
// 不带前缀在鸿蒙上返回 EOPNOTSUPP(95)。这是**命名约定**的差异，不是语义差异，
// 所以在垫片里统一补上 user. 前缀——上游的属性名照用，落到盘上带前缀。
// Linux requires a namespace prefix on xattr names (user./security./system./trusted.); Apple allows any
// name and upstream uses "org.d2rmobile.guest-name", which returns EOPNOTSUPP(95) here. That is a naming
// convention, not a semantic difference, so the shim prefixes "user." while keeping upstream's name.
static void xattr_name(const char *name, char *out, size_t n) {
    if (!strncmp(name, "user.", 5) || !strncmp(name, "security.", 9) ||
        !strncmp(name, "system.", 7) || !strncmp(name, "trusted.", 8)) {
        snprintf(out, n, "%s", name);
        return;
    }
    snprintf(out, n, "user.%s", name);
}

static ssize_t xattr_get_norm(const char *path, const char *name, void *value, size_t size, int nofollow) {
    char nm[256];
    xattr_name(name, nm, sizeof nm);
    ssize_t r = (ssize_t)syscall(nofollow ? SYS_lgetxattr : SYS_getxattr, path, nm, value, size);
    if (r < 0 && errno == EOPNOTSUPP) errno = ENODATA;
    return r;
}

ssize_t dm_compat_getxattr(const char *path, const char *name, void *value, size_t size,
                           uint32_t position, int options) {
    if (position != 0) { errno = EINVAL; return -1; }
    return xattr_get_norm(path, name, value, size, (options & XATTR_NOFOLLOW) != 0);
}

ssize_t dm_compat_setxattr(const char *path, const char *name, const void *value, size_t size,
                           uint32_t position, int options) {
    if (position != 0) { errno = EINVAL; return -1; }
    char nm[256];
    xattr_name(name, nm, sizeof nm);
    return (ssize_t)syscall((options & XATTR_NOFOLLOW) ? SYS_lsetxattr : SYS_setxattr,
                            path, nm, value, size, 0 /* flags */);
}

int dm_compat_removexattr(const char *path, const char *name, int options) {
    char nm[256];
    xattr_name(name, nm, sizeof nm);
    return (int)syscall((options & XATTR_NOFOLLOW) ? SYS_lremovexattr : SYS_removexattr, path, nm);
}

// ================================================================ malloc 统计：如实给 0

// musl 没有苹果的 malloc zone，也没有稳定的等价接口。这里**不编数字**：字段全填 0，
// 并打一次明确说明。理由：这个数字出现在"长时间跑机"的内存观测表里，
// 填一个像真的假数字，会让人在排查内存问题时被误导；填 0 加一句说明，看的人立刻知道"这项不可用"。
// musl has no malloc zones and no stable equivalent. We do not invent numbers: everything is zero with a
// one-off explanation. This value feeds the long-run memory observation table, where a plausible fake
// number would mislead whoever is chasing a memory problem, while a zero plus a note says "not available".
void malloc_zone_statistics(malloc_zone_t *zone, malloc_statistics_t *stats) {
    (void)zone;
    if (!stats) return;
    memset(stats, 0, sizeof *stats);
    static int warned;
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[dm] 【说明】malloc_zone_statistics：musl 无等价接口，本项统计在鸿蒙上不可用（填 0）\n");
    }
}
