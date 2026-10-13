// 垫片实现：strlcpy / strlcat / arc4random 系列 / renamex_np。
// Shim implementation: strlcpy / strlcat / arc4random family / renamex_np.
//
// 全部用 __attribute__((weak)) 定义：万一鸿蒙 libc 将来自带同名符号，以 libc 的为准，不会重复定义。
// All defined __attribute__((weak)): if the HarmonyOS libc ever ships the same symbol, libc's version wins.
#include "shim_prefix.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

// ---------------------------------------------------------------- strlcpy / strlcat
// 语义完全照苹果：返回"本来需要多少字节"（不含结尾 0），超过则截断。
// 调用方用这个返回值判断有没有截断——回归里也依赖它，所以不能图省事返回别的。
// Semantics exactly as Apple's: returns the length the result would have needed (excluding the NUL), and
// truncates on overflow. Callers rely on that value to detect truncation, so it must not be faked.

__attribute__((weak)) size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t slen = strlen(src);
    if (size) {
        size_t n = slen < size - 1 ? slen : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return slen;
}

__attribute__((weak)) size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strnlen(dst, size);
    if (dlen == size) return size + strlen(src);      // 目标串没有结尾 0：苹果也这么算 | dst not terminated: Apple computes it the same way
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}

// ---------------------------------------------------------------- arc4random
// 宿主拿它当熵源（hle_os.c 的随机数）、取名字（dm_namegen.c）、秘境随机（dm_rift.c）。
// 游戏的对拍模式（DM_DETCLOCK）会绕开真随机、走固定种子流，所以这里只要"真随机且不掉链子"。
// The host uses this for entropy, name generation and rift randomness. The game's comparison mode
// (DM_DETCLOCK) bypasses real randomness with a fixed-seed stream, so this only has to be genuinely
// random and never fail.
//
// 取数顺序：getrandom 系统调用 → /dev/urandom → 最后才退回时间种子（并打一次红字）。
// 最后那条路只保证"不卡住、不返回固定值"，不保证密码学强度；一旦走到它，日志里会有明确提示。
// Order: getrandom syscall, then /dev/urandom, and only last a time-seeded fallback with a loud note.
// The fallback guarantees only "does not hang, is not constant", not cryptographic strength.
static int dm_rand_bytes(void *buf, size_t n) {
#ifdef SYS_getrandom
    {
        uint8_t *p = (uint8_t *)buf; size_t left = n;
        while (left) {
            long r = syscall(SYS_getrandom, p, left, 0);
            if (r > 0) { p += r; left -= (size_t)r; continue; }
            if (r < 0 && errno == EINTR) continue;
            break;                                     // ENOSYS / EPERM 等：换下一条路 | ENOSYS / EPERM etc.: try the next route
        }
        if (!left) return 0;
    }
#endif
    {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            uint8_t *p = (uint8_t *)buf; size_t left = n; int ok = 1;
            while (left) {
                ssize_t r = read(fd, p, left);
                if (r > 0) { p += r; left -= (size_t)r; continue; }
                if (r < 0 && errno == EINTR) continue;
                ok = 0; break;
            }
            close(fd);
            if (ok) return 0;
        }
    }
    return -1;
}

// 兜底流：只在上面两条路都断了时才用。刻意留成"能看出来是兜底"的实现。
// Last resort, used only when both routes above are dead; deliberately left visibly makeshift.
static uint64_t dm_fallback_state;
static uint32_t dm_fallback_next(void) {
    if (!dm_fallback_state) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        dm_fallback_state = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^ (uint64_t)getpid();
        if (!dm_fallback_state) dm_fallback_state = 0x9E3779B97F4A7C15ull;
    }
    uint64_t z = (dm_fallback_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}

__attribute__((weak)) void arc4random_buf(void *buf, size_t n) {
    static int warned;
    if (dm_rand_bytes(buf, n) == 0) return;
    if (!warned) { warned = 1; fprintf(stderr, "[dm] 【红】取不到系统熵源（getrandom / /dev/urandom 都失败）：退到时间种子的伪随机，随机性不保证\n"); }
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = dm_fallback_next();
        size_t k = n - i < 4 ? n - i : 4;
        memcpy(p + i, &v, k);
    }
}

__attribute__((weak)) uint32_t arc4random(void) {
    uint32_t v;
    arc4random_buf(&v, sizeof v);
    return v;
}

// 无模偏差的 [0, upper_bound)：与苹果同样的拒绝采样，保证分布均匀。
// Unbiased [0, upper_bound) via the same rejection sampling Apple uses.
__attribute__((weak)) uint32_t arc4random_uniform(uint32_t upper_bound) {
    if (upper_bound < 2) return 0;
    uint32_t min = (uint32_t)(-upper_bound) % upper_bound;   // 2^32 % upper_bound
    for (;;) {
        uint32_t r = arc4random();
        if (r >= min) return r % upper_bound;
    }
}

// ---------------------------------------------------------------- renamex_np
// 宿主只用一次（hle_fs.c 的存档提交）：把工作目录与已提交目录**原子互换**。
// 苹果靠 renamex_np(RENAME_SWAP)；Linux 上的对应物是 renameat2(RENAME_EXCHANGE)。
// 若内核不支持（ENOSYS / EINVAL），退回"三步改名"，并明确打一次提示：
// 那条路**不是原子的**——极端时点被杀可能留下一个中间态，但绝不会丢数据（三个目录来回搬名字）。
// Used once (save commit in hle_fs.c) for an atomic swap. The Linux counterpart of RENAME_SWAP is
// renameat2(RENAME_EXCHANGE). Without kernel support it falls back to a three-step rename and says so:
// that path is not atomic (a kill at the worst moment can leave an intermediate state) but never loses
// data, because only names move.
__attribute__((weak)) int renamex_np(const char *from, const char *to, unsigned int flags) {
    if (!from || !to) { errno = EINVAL; return -1; }
#ifdef SYS_renameat2
    if (flags == RENAME_SWAP) {
        long r = syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 2 /* RENAME_EXCHANGE */);
        if (r == 0) return 0;
        if (errno != ENOSYS && errno != EINVAL) return -1;
    } else if (flags == RENAME_EXCL) {
        long r = syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, 1 /* RENAME_NOREPLACE */);
        if (r == 0) return 0;
        if (errno != ENOSYS && errno != EINVAL) return -1;
    }
#else
    (void)flags;
#endif
    if (flags & RENAME_EXCL) {
        if (access(to, F_OK) == 0) { errno = EEXIST; return -1; }
    }
    if (!(flags & RENAME_SWAP)) return rename(from, to);
    {
        static int warned;
        if (!warned) { warned = 1; fprintf(stderr, "[dm] 【红】本机不支持 renameat2(RENAME_EXCHANGE)：存档提交退回非原子三步改名\n"); }
        char tmp[4096];
        if (snprintf(tmp, sizeof tmp, "%s.dmswap", to) >= (int)sizeof tmp) { errno = ENAMETOOLONG; return -1; }
        if (rename(to, tmp)) return -1;
        if (rename(from, to)) { int e = errno; rename(tmp, to); errno = e; return -1; }
        if (rename(tmp, from)) return -1;
        return 0;
    }
}
