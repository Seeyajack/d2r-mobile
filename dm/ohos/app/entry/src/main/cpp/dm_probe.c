// 三个真机实验的实现。见 dm_probe.h。
#define _GNU_SOURCE

#include "dm_probe.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "dm.h"                 // module_t
#include "map_image_ohos.h"     // 鸿蒙版 map_image —— 实验 D 的被测对象

// ───────────────────────── 输出小工具 ─────────────────────────

// 每写一行，同时打进 hilog —— 万一界面没显示出来（或进程中途挂了），
// 也能从设备日志里看到跑到哪一步了。
static void logline(const char *s) {
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xD001, "dmprobe", "%{public}s", s);
}

static void app(char *out, size_t cap, size_t *used, const char *s) {
    logline(s);
    size_t n = strlen(s);
    if (*used + n + 1 > cap) n = (cap > *used + 1) ? cap - *used - 1 : 0;
    memcpy(out + *used, s, n);
    *used += n;
    out[*used] = 0;
}

static void appf(char *out, size_t cap, size_t *used, const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    app(out, cap, used, line);
}

// ──────────── 实验 C 的素材：一个自定义 ELF 段 ────────────
// 段名必须是合法的 C 标识符，链接器才会为它生成 __start_<段名> / __stop_<段名>。
// used 防止被 --gc-sections 丢掉。

__attribute__((section("dmprobesec"), used))
const char dm_probe_marker[] = "DM_PROBE_MARKER_0123456789ABCDEF";

extern const char __start_dmprobesec[];
extern const char __stop_dmprobesec[];

// 实验 B 用：本文件里的一个符号，用来反查本模块 .so 的路径
static void probe_self_symbol(void) {}

// ─────────────────────── 实验 A ───────────────────────

// 执行测试用的信号兜底：页不可执行时 CPU 抛 SIGSEGV/SIGBUS，
// 处理器跳回来，进程不至于挂掉。
static sigjmp_buf g_probe_jmp;
static volatile sig_atomic_t g_probe_jmp_armed;

static void probe_segv_handler(int sig) {
    if (g_probe_jmp_armed) {
        g_probe_jmp_armed = 0;
        siglongjmp(g_probe_jmp, 1);
    }
    _exit(128 + sig);   // 没武装：交回默认处理
}

static void exp_a(char *out, size_t cap, size_t *used) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);

    app(out, cap, used, "【实验 A】匿名内存能否拿到可执行权限\n");
    app(out, cap, used, "  预期：失败（鸿蒙自 5.0.0(12) 起禁止匿名内存可执行）\n");
    appf(out, cap, used, "  页大小 = %zu 字节\n", pg);

    void *p = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        appf(out, cap, used, "  ✗ mmap 匿名 RW 就失败了：%s\n", strerror(errno));
        app(out, cap, used, "  结论：连匿名内存都拿不到，情况比预期更严\n\n");
        return;
    }
    appf(out, cap, used, "  ✓ mmap 匿名 RW：成功，地址 %p\n", p);

    // 顺序要紧：先把指令写进去，再 mprotect 成 R+X。
    // 反过来（先 mprotect 再写）会往只读页写 → 当场 SIGSEGV（第一次跑就死在这儿）。
    memcpy(p, "\xc0\x03\x5f\xd6", 4);   // ARM64: ret

    if (mprotect(p, pg, PROT_READ | PROT_EXEC) != 0) {
        appf(out, cap, used, "  ✓ mprotect 成 R+X：失败（%s）\n", strerror(errno));
        app(out, cap, used, "  结论：符合预期。匿名可执行内存被禁 → 桌面版 map_image() 的写法"
                            "在鸿蒙上行不通，必须走文件映射路线。\n\n");
        munmap(p, pg);
        return;
    }

    // mprotect 说成功 —— 但那不一定代表页真的可执行（有些系统在缺页时才拦）。
    // 真执行一次才算数。
    //
    // 隔离方式：不用 fork()（实测在鸿蒙 App 里 fork 会把整个进程带崩），
    // 改用信号处理器 + siglongjmp：真崩了就跳回来，进程继续活。
    app(out, cap, used, "  ⚠ mprotect 成 R+X：成功  ← 与预期不符！再验一下是否真能执行\n");

    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_segv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    if (sigsetjmp(g_probe_jmp, 1) == 0) {
        g_probe_jmp_armed = 1;
        ((void (*)(void))p)();
        g_probe_jmp_armed = 0;
        app(out, cap, used, "  ⚠⚠ 匿名内存真的可执行！桌面版写法可用，方案可以简化\n");
    } else {
        app(out, cap, used, "  ✓ 执行时崩了（SIGSEGV/SIGBUS）→ 页其实不可执行，"
                            "mprotect 只是没当场拦\n");
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);

    app(out, cap, used, "\n");
    munmap(p, pg);
}

// ─────────────────────── 实验 B ───────────────────────

static void exp_b(const char *files_dir, char *out, size_t cap, size_t *used) {
    size_t pg = (size_t)sysconf(_SC_PAGESIZE);

    app(out, cap, used, "【实验 B】mmap 自己的 .so 文件能否拿到可执行权限\n");
    app(out, cap, used, "  预期：成功 —— 这是整个装载方案的基石\n");

    Dl_info info;
    if (!dladdr((void *)probe_self_symbol, &info) || !info.dli_fname) {
        app(out, cap, used, "  ✗ dladdr 取不到本模块路径\n\n");
        return;
    }
    appf(out, cap, used, "  本模块 .so 路径：%s\n", info.dli_fname);

    int fd = open(info.dli_fname, O_RDONLY);
    if (fd < 0) {
        appf(out, cap, used, "  ✗ 打不开：%s\n\n", strerror(errno));
        return;
    }

    // B-1：最简单的形式 —— 文件页直接映射成 R+X
    void *q = mmap(NULL, pg, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    int b1_ok = (q != MAP_FAILED);
    if (!b1_ok) {
        appf(out, cap, used, "  ✗ B-1 mmap bundle 里的 .so 成 R+X：失败（%s）\n", strerror(errno));
    } else {
        appf(out, cap, used, "  ✓ B-1 mmap bundle 里的 .so 成 R+X：成功，地址 %p\n", q);
        munmap(q, pg);

        // B-2：完整机制演练 —— 先预留连续地址，再把文件页 MAP_FIXED 进去
        //     这正是替代 iOS 的 vm_allocate + vm_remap 的做法
        size_t total = pg * 4;
        void *res = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (res == MAP_FAILED) {
            appf(out, cap, used, "  ✗ B-2 预留地址失败：%s\n", strerror(errno));
        } else {
            void *want = (char *)res + pg;
            void *got = mmap(want, pg, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_FIXED, fd, 0);
            if (got == MAP_FAILED)
                appf(out, cap, used, "  ✗ B-2 MAP_FIXED 进预留区：失败（%s）\n", strerror(errno));
            else if (got != want)
                appf(out, cap, used, "  ✗ B-2 落在了别处：想要 %p，得到 %p\n", want, got);
            else
                app(out, cap, used, "  ✓ B-2 预留 + MAP_FIXED：成功 —— "
                                    "「按原相对位置摆回去」的机制可用\n");
            munmap(res, total);
        }
    }
    close(fd);

    // B-3：换一个「应用数据目录」里的文件再试一次。
    // 用来分辨：禁令是针对 bundle 目录（.so 所在处），还是所有应用文件都不许可执行映射。
    // 这决定了方案是"换个地方放镜像"就行，还是要整个换路线。
    int b3_ok = 0, b3_tried = 0;
    if (files_dir && *files_dir) {
        b3_tried = 1;
        char tpath[1024];
        snprintf(tpath, sizeof tpath, "%s/probe_b3.bin", files_dir);
        int tf = open(tpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (tf < 0) {
            appf(out, cap, used, "  ✗ B-3 数据目录建文件失败（%s）：%s\n", tpath, strerror(errno));
        } else {
            char buf[4096];
            memset(buf, 0, sizeof buf);
            memcpy(buf, "\xc0\x03\x5f\xd6", 4);   // ARM64: ret
            if (write(tf, buf, sizeof buf) != (ssize_t)sizeof buf) {
                appf(out, cap, used, "  ✗ B-3 写测试文件失败：%s\n", strerror(errno));
            } else {
                void *r = mmap(NULL, pg, PROT_READ | PROT_EXEC, MAP_PRIVATE, tf, 0);
                if (r == MAP_FAILED) {
                    appf(out, cap, used, "  ✗ B-3 mmap 数据目录文件成 R+X：失败（%s）\n",
                         strerror(errno));
                } else {
                    b3_ok = 1;
                    appf(out, cap, used, "  ✓ B-3 mmap 数据目录文件成 R+X：成功，地址 %p\n", r);
                    munmap(r, pg);
                }
            }
            close(tf);
        }
    }

    // 汇总
    if (b1_ok) {
        app(out, cap, used, "  结论：文件映射路线成立，装载方案按原设计推进。\n");
    } else if (b3_ok) {
        app(out, cap, used, "  结论：bundle 目录不许可执行映射，但数据目录可以 →\n"
                            "        把镜像放到应用数据目录再映射即可，方案仍然成立。\n");
    } else if (b3_tried) {
        app(out, cap, used, "  结论：bundle 与数据目录都不许 → 文件映射路线整体不可用，\n"
                            "        必须换方案（候选：镜像放进 .so 自己的可执行段，\n"
                            "        用链接脚本强制三块在文件里相邻，不搬位置）。\n");
    } else {
        app(out, cap, used, "  结论：B-3 未跑（没拿到数据目录路径），无法判断禁令范围。\n");
    }
    app(out, cap, used, "\n");
}

// ─────────────────────── 实验 C ───────────────────────

static void exp_c(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 C】自定义 ELF 段能否生成 __start_/__stop_ 符号\n");
    app(out, cap, used, "  预期：能（lld 的标准行为）\n");

    size_t n = (size_t)(__stop_dmprobesec - __start_dmprobesec);
    appf(out, cap, used, "  __start_dmprobesec = %p\n", (const void *)__start_dmprobesec);
    appf(out, cap, used, "  __stop_dmprobesec  = %p\n", (const void *)__stop_dmprobesec);
    appf(out, cap, used, "  段长度 = %zu 字节\n", n);

    if (n == 0) {
        app(out, cap, used, "  ✗ 段长度为 0 → 链接器没生成符号，或段被丢掉了\n");
        app(out, cap, used, "  结论：要用退路（运行时解析 ELF 头，或链接脚本强制布局）\n");
    } else if (n >= 32 && memcmp(__start_dmprobesec, "DM_PROBE_MARKER", 15) == 0) {
        appf(out, cap, used, "  段内容前 16 字节：%.16s\n", __start_dmprobesec);
        app(out, cap, used, "  ✓ 符号可用，内容与标记匹配\n");
        app(out, cap, used, "  结论：嵌入用「自定义段 + 链接器符号」可行，"
                            "不必自己解析 ELF 头。\n");
    } else {
        app(out, cap, used, "  ⚠ 段有长度，但内容对不上标记 —— 需要细看\n");
    }
    app(out, cap, used, "\n");
}

// ─────────── 实验 D：完整链路（嵌入 → 定位 → 映射 → 执行）───────────
// 用 gen_embed_ohos.py 生成的 test_image.S 当"游戏映像"：
//   偏移 0x0000  代码 mov w0,#0x1234 ; ret
//   偏移 0x4000  数据 "DM_OHOS_IMAGE_OK"
//   TEXT_END 0x2000 / 键页 0x4000 / PATCH_OFF 0x8000 / 共 0xC000
// 走的是真正的 dm_ohos_map_image_alloc/seal（dm/ohos/map_image_ohos.c）。
static void exp_d(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 D】完整链路：嵌入 → 定位 → 映射 → 执行\n");
    app(out, cap, used, "  用 gen_embed_ohos.py 生成的测试映像走真正的 map_image\n");

    module_t m;
    memset(&m, 0, sizeof m);
    strncpy(m.name, "main", sizeof m.name - 1);
    m.text_end = 0x2000;
    m.patch_off = 0x8000;
    m.key_page_off = 0x4000;
    m.key_page_size = 0x4000;

    if (dm_ohos_map_image_alloc(&m)) {
        app(out, cap, used, "  ✗ alloc 失败（见 hilog）\n\n");
        return;
    }
    appf(out, cap, used, "  ✓ 定位 + 拷贝：base %p，size %zu\n", (void *)m.base, m.size);

    // 封代码区之前，先确认数据段读得到（w 段落在映像偏移 0x4000）
    const char *magic = (const char *)(m.base + 0x4000);
    int data_ok = (memcmp(magic, "DM_OHOS_IMAGE_OK", 16) == 0);
    appf(out, cap, used, "  %s 数据段：\"%.16s\"\n", data_ok ? "✓" : "✗", magic);

    if (dm_ohos_map_image_seal(&m)) {
        app(out, cap, used, "  ✗ seal 失败（见 hilog）\n\n");
        return;
    }
    app(out, cap, used, "  ✓ 代码区已封为只读+可执行\n");

    // 真执行映像偏移 0 处的代码，应返回 0x1234
    int r = ((int (*)(void))m.base)();
    int code_ok = (r == 0x1234);
    appf(out, cap, used, "  %s 执行映像代码：返回 0x%x（期望 0x1234）\n",
         code_ok ? "✓" : "✗", (unsigned)r);

    if (code_ok && data_ok)
        app(out, cap, used, "  结论：整条链路通了 —— 嵌入/定位/映射/执行全部成立。\n\n");
    else
        app(out, cap, used, "  结论：链路有问题，需细看（见 hilog）。\n\n");
}

// ───────── 实验 E：musl 的线程槽布局（决定改写规则怎么改）─────────
// 整套改写方案只依赖一条等式：线程槽地址 = tpidrro_el0 + 键号 × 8
// （苹果的 pthread 把 TSD 放在 tpidrro_el0 指向的结构里）。
// 这里把 musl 的真实情况测出来：两个线程指针寄存器各是什么，
// pthread_setspecific 写的值落在 tpidr_el0 的哪个偏移上，键号是不是线性 ×8。
static void exp_e(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 E】musl 的线程槽布局\n");
    app(out, cap, used, "  要验的等式：槽地址 = tpidrro_el0 + 键号×8（苹果 pthread 的布局）\n");

    uint64_t tp_el0 = 0, tp_ro = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp_el0));
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp_ro));
    appf(out, cap, used, "  tpidr_el0   = 0x%llx\n", (unsigned long long)tp_el0);
    appf(out, cap, used, "  tpidrro_el0 = 0x%llx%s\n", (unsigned long long)tp_ro,
         tp_ro ? "" : "   ← 为 0！苹果方案靠它取基址，这里取不到");

    #define NK 4
    pthread_key_t k[NK];
    const uint64_t marker[NK] = { 0x1111111111111111ull, 0x2222222222222222ull,
                                  0x3333333333333333ull, 0x4444444444444444ull };
    for (int i = 0; i < NK; i++) {
        if (pthread_key_create(&k[i], NULL)) {
            appf(out, cap, used, "  ✗ 建第 %d 个键失败\n\n", i);
            return;
        }
        pthread_setspecific(k[i], (void *)(uintptr_t)marker[i]);
    }
    appf(out, cap, used, "  建了 %d 个键，键号：%lu %lu %lu %lu\n", NK,
         (unsigned long)k[0], (unsigned long)k[1], (unsigned long)k[2], (unsigned long)k[3]);

    // 在 tpidr_el0 附近扫，看每个 marker 落在哪个偏移。
    // 整段扫描包在一个信号兜底里：万一越界，进程不至于挂。
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_segv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    long off[NK] = { 0 };
    int faulted = 0;
    if (sigsetjmp(g_probe_jmp, 1) == 0) {
        g_probe_jmp_armed = 1;
        for (int i = 0; i < NK; i++) {
            off[i] = 0;
            for (long o = -0x2000; o < 0x8000; o += 8) {
                uint64_t v = *(const uint64_t *)(uintptr_t)(tp_el0 + (uint64_t)o);
                if (v == marker[i]) { off[i] = o; break; }
            }
        }
        g_probe_jmp_armed = 0;
    } else {
        faulted = 1;
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);

    if (faulted) {
        app(out, cap, used, "  ⚠ 扫描时越界，结果不完整\n\n");
        return;
    }

    for (int i = 0; i < NK; i++) {
        if (off[i]) {
            appf(out, cap, used, "  键号 %-4lu → 值落在 tpidr_el0 %+ld（0x%lx）\n",
                 (unsigned long)k[i], off[i], (unsigned long)off[i]);
        } else {
            appf(out, cap, used, "  键号 %-4lu → 在 tpidr_el0-0x2000..+0x8000 内没找到\n",
                 (unsigned long)k[i]);
        }
    }

    // 键号相邻的两个槽，偏移差应该是 8（说明是数组）
    if (off[0] && off[1]) {
        long d = off[1] - off[0];
        long dk = (long)k[1] - (long)k[0];
        appf(out, cap, used, "  相邻键的偏移差 %ld，键号差 %ld → 每键 %s\n",
             d, dk, (dk && d == dk * 8) ? "8 字节（是数组）" : "不是 8 字节，要细看");
        if (dk && d == dk * 8) {
            appf(out, cap, used, "  结论：槽地址 = tpidr_el0 + %ld + 键号×8\n",
                 off[0] - (long)k[0] * 8);
            app(out, cap, used, "        → 改写器把 tpidrro_el0 换成 tpidr_el0，\n"
                                "          键页里写 (上面那个常数 + 键号×8) 即可。\n");
        }
    }
    app(out, cap, used, "\n");
}

// ─────────────────────── 入口 ───────────────────────

int dm_probe_run(const char *files_dir, char *out, size_t cap) {
    if (!out || cap < 64) return -1;
    size_t used = 0;
    out[0] = 0;

    app(out, cap, &used, "D2R Mobile · 鸿蒙移植 · 第 1 站实验\n");
    app(out, cap, &used, "==================================================\n\n");
    if (files_dir && *files_dir) {
        appf(out, cap, &used, "应用数据目录：%s\n\n", files_dir);
    }

    // 这几条只进 hilog、不进报告：万一某一步把进程搞挂了，
    // 从日志里就能看出死在哪一步（有"进入"没"返回"）。
    // 顺序有讲究：A 的最后一步要真执行一段可能不可执行的内存，
    // 万一信号兜底没兜住、进程挂了，B/C 也已经跑完并写进日志了。
    logline(">>> 实验 D 开始"); exp_d(out, cap, &used); logline(">>> 实验 D 返回");
    logline(">>> 实验 E 开始"); exp_e(out, cap, &used); logline(">>> 实验 E 返回");
    logline(">>> 实验 C 开始"); exp_c(out, cap, &used); logline(">>> 实验 C 返回");
    logline(">>> 实验 B 开始"); exp_b(files_dir, out, cap, &used); logline(">>> 实验 B 返回");
    logline(">>> 实验 A 开始"); exp_a(out, cap, &used); logline(">>> 实验 A 返回");

    app(out, cap, &used, "==================================================\n");
    app(out, cap, &used, "把这份报告发回来，就能定下装载方案。\n");
    logline(">>> 三个实验全部跑完");
    return 0;
}
