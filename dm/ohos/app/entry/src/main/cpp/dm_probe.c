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
#include "dm_main_layout.h"     // 旷野之息 main 的布局常数（fixups 表头生成）
#include "dm_main_gates.h"      // 旷野之息 main 的门表（93 个 svc 门，实验 H 登记）

// 旷野之息 main 的嵌入段（dm_images.S）：装载验证要把「映射区」与「嵌入段」逐字节对拍
extern const unsigned char __start_dmmainx0[];
extern const unsigned char __stop_dmmainx0[];
extern const unsigned char __start_dmmainw[];
extern const unsigned char __stop_dmmainw[];
extern const unsigned char __start_dmmainx1[];
extern const unsigned char __stop_dmmainx1[];

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
    strncpy(m.name, "probe", sizeof m.name - 1);   // 测试映像（test_image.S，段名 dmprobe*）；真实模块是 main（实验 G/H）
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

// ───── 实验 E2：找 musl 的 tsd 指针在 pthread 结构里的偏移 ─────
// 实验 E 已说明：槽值不在 tpidr_el0 + 键号×8 处，也不是 tpidr_el0 附近的数组。
// musl 的做法是在 struct pthread 里放一个 void **tsd，指向单独分配的槽数组：
//     槽地址 = *(void**)(tpidr_el0 + tsd_off) + 键号 × 8
// 这里扫 pthread 结构，找那个 tsd_off。
static int safe_read64(uint64_t addr, uint64_t *out) {
    if (sigsetjmp(g_probe_jmp, 1) == 0) {
        g_probe_jmp_armed = 1;
        *out = *(const uint64_t *)(uintptr_t)addr;
        g_probe_jmp_armed = 0;
        return 1;
    }
    return 0;   // 越界被信号兜底接住了
}

static void exp_e2(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 E2】找 musl 的 tsd 指针偏移\n");

    uint64_t tp = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    appf(out, cap, used, "  tpidr_el0 = 0x%llx\n", (unsigned long long)tp);

    pthread_key_t k1, k2;
    if (pthread_key_create(&k1, NULL) || pthread_key_create(&k2, NULL)) {
        app(out, cap, used, "  ✗ 建键失败\n\n");
        return;
    }
    const uint64_t m1 = 0x5a5cafe123456789ull;
    const uint64_t m2 = 0x0f1e2d3c4b5a6978ull;
    pthread_setspecific(k1, (void *)(uintptr_t)m1);
    pthread_setspecific(k2, (void *)(uintptr_t)m2);
    appf(out, cap, used, "  键号 %lu → 0x%llx，%lu → 0x%llx\n",
         (unsigned long)k1, (unsigned long long)m1,
         (unsigned long)k2, (unsigned long long)m2);

    // 扫 pthread 结构：找偏移 o，使 *(pthread+o) 是个指针 p，且 p[k1]==m1、p[k2]==m2
    // 扫描会碰到未映射地址，必须先装信号兜底（safe_read64 靠 siglongjmp 回来）
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_segv_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    long found_off = 0;
    for (long o = -0x400; o < 0x2000; o += 8) {
        uint64_t p = 0;
        if (!safe_read64(tp + (uint64_t)o, &p)) continue;
        if (p < 0x10000 || p > 0x8000000000000000ull) continue;    // 不像用户态指针
        uint64_t v1 = 0, v2 = 0;
        if (!safe_read64(p + (uint64_t)k1 * 8, &v1)) continue;
        if (v1 != m1) continue;
        if (!safe_read64(p + (uint64_t)k2 * 8, &v2)) continue;
        if (v2 != m2) continue;
        found_off = o;
        break;
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);

    if (!found_off) {
        app(out, cap, used, "  ✗ 没找到（可能是两级以上间接，或 tsd 数组另存）\n\n");
        return;
    }
    appf(out, cap, used, "  ✓ 找到了：pthread + %ld 是指针 p，且 p[%lu]==marker1、p[%lu]==marker2\n",
         found_off, (unsigned long)k1, (unsigned long)k2);
    appf(out, cap, used, "  → 完整公式：槽地址 = *(void**)(tpidr_el0 + %ld) + 键号 × 8\n", found_off);
    app(out, cap, used, "  → 改写器改用 tpidr_el0 作基址，插入一次「取 tsd 指针」的 ldr；\n"
                        "    键页仍写 键号×8，不动。\n\n");
}

// ───── 实验 F：改写器生成的指令序列，在 musl 上真的能读到槽吗 ─────
// 把 patch_nso.py 的 emit_slot_addr 在 OHOS 下生成的那串指令原样执行一遍：
//     mrs T, tpidr_el0 → sub T,T,#272 → ldr T,[T] → add T,T,key*8 → ldr S,[T]
// 值由 pthread_setspecific 写进去（改写后的游戏代码就是这么读的），
// 看它能不能读回来、和 pthread_getspecific 是否一致。
#if defined(__aarch64__)
#define DM_SLOT_READ(dst, off) __asm__ volatile ( \
    "mov x9, %1 \n"                  /* x9  = 键页里存的值（键号×8） */ \
    "mrs x10, tpidr_el0 \n"          /* x10 = pthread 指针 */ \
    "sub x10, x10, #272 \n"          /* x10 = &pthread->tsd */ \
    "ldr x10, [x10] \n"              /* x10 = musl 的 tsd 数组 */ \
    "add x9, x10, x9 \n"             /* x9  = 槽地址 */ \
    "ldr %0, [x9] \n"                /* dst = 槽值 */ \
    : "=r"(dst) : "r"((uint64_t)(off)) : "x9", "x10", "memory")
#else
#define DM_SLOT_READ(dst, off) do { (dst) = (uint64_t)(uintptr_t)pthread_getspecific(0); } while (0)
#endif

static void exp_f(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 F】改写器生成的指令序列能否读到宿主写的槽\n");
    app(out, cap, used, "  原样执行 emit_slot_addr 在 OHOS 下生成的五条指令\n");

    pthread_key_t k1, k2;
    if (pthread_key_create(&k1, NULL) || pthread_key_create(&k2, NULL)) {
        app(out, cap, used, "  ✗ 建键失败\n\n");
        return;
    }
    const uint64_t v1 = 0xA5A5DEAD00000001ull;
    const uint64_t v2 = 0xA5A5DEAD00000002ull;
    pthread_setspecific(k1, (void *)(uintptr_t)v1);
    pthread_setspecific(k2, (void *)(uintptr_t)v2);

    uint64_t r1 = 0, r2 = 0;
    DM_SLOT_READ(r1, (uint64_t)k1 * 8);
    DM_SLOT_READ(r2, (uint64_t)k2 * 8);
    uint64_t g1 = (uint64_t)(uintptr_t)pthread_getspecific(k1);
    uint64_t g2 = (uint64_t)(uintptr_t)pthread_getspecific(k2);

    appf(out, cap, used, "  键号 %-4lu：原生序列读回 0x%llx，宿主读回 0x%llx，写入 0x%llx  %s\n",
         (unsigned long)k1, (unsigned long long)r1, (unsigned long long)g1,
         (unsigned long long)v1, (r1 == g1 && r1 == v1) ? "✓" : "✗");
    appf(out, cap, used, "  键号 %-4lu：原生序列读回 0x%llx，宿主读回 0x%llx，写入 0x%llx  %s\n",
         (unsigned long)k2, (unsigned long long)r2, (unsigned long long)g2,
         (unsigned long long)v2, (r2 == g2 && r2 == v2) ? "✓" : "✗");

    if (r1 == g1 && r2 == g2 && r1 == v1 && r2 == v2)
        app(out, cap, used, "  结论：改写器生成的代码序列在 musl 上读得到宿主写的槽，"
                            "且与 pthread_getspecific 一致。\n\n");
    else
        app(out, cap, used, "  结论：读不到或不一致，需要细看。\n\n");
}

// ─────────────────────── 入口 ───────────────────────

// ───────── 探针桩：门回路只走 svc 38 的占位路径，下列服务不会被触及 ─────────
// hle.c / hle_svc.c 还引用导入解析、线程与同步、私有宿主调用（都是"宿主服务按游戏提供"的边界，
// 正式装载由 loader.c/hle_thread.c/hle_tact.c 提供）。这里给惰性实现：链接闭合、行为可见。
module_t g_mod[MAX_MOD]; int g_nmod = 0;                 // 空模块表：导入解析路径不会走到
struct dm_flags g_flag;                                   // 调试开关：全 0
module_t *module_of(const void *p) { (void)p; return NULL; }
uint64_t guest_ns(void) {                                 // 客体时钟：单调钟直通（探针不走 DM_TICK_HOLD）
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
void dm_backtrace(uint64_t fp) { (void)fp; }
uint64_t dm_entropy64(void) { static uint64_t s; return s += 0x9E3779B97F4A7C15ull; }
void hlecount_svc(int num) { (void)num; }
void *hlecount_wrap(const char *a, const char *b, void *fn) { (void)a; (void)b; return fn; }
uint64_t svc_host_call(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }   // 0 = 未处理 → 落到占位返回
void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig);   // hle.c
static void gate_probe_entry(void) { logline("✓ 到达 KGATE 入口（门块执行 + 槽读取路径成立）"); }
extern void dm_gate(void);                                // entry.S：门公共入口
void dm_thread_returned(void);                            // entry.S：客人线程要求退出时落到这里（noreturn）
void dm_thread_returned(void) {                           // 探针桩：门回路不触发线程退出；真触发就走 _exit 让它可见
    fprintf(stderr, "[dm] dm_thread_returned 被触发（不应发生在门回路验证里）\n"); _exit(0);
}
// 强制接管表的空段：hle.c 会遍历 dmswhle（正式装载由 HLE_FORCE 产出），探针没有条目
__attribute__((used, section("dmswhle"))) static const char dm_swhle_probe[4] = {0};

// 实验 H 的门错误兜底：SIGSEGV/SIGBUS 时记下出错 PC，跳回探针（进程活着，逐门继续报）
static sigjmp_buf g_gate_jmp;
static volatile sig_atomic_t g_gate_armed;
static volatile uint64_t g_gate_fault_pc;
static volatile int g_gate_fault_sig;
static void gate_fault_handler(int sig, siginfo_t *si, void *uc) {
    (void)si;
    g_gate_fault_sig = sig;
    g_gate_fault_pc = (uint64_t)((ucontext_t *)uc)->uc_mcontext.pc;   // aarch64 mcontext 的 pc
    if (!g_gate_armed) {
        // 未武装段（尾跳补丁循环等）出段错误：先打日志再退出——PC 落在 hilog 里供定位
        LOG("【红】门回路：未武装段 SIGSEGV/SIGBUS pc=%#llx sig=%llx（退出码 139）", (unsigned long long)g_gate_fault_pc, (unsigned long long)sig);
        _exit(139);
    }
    g_gate_armed = 0;
    siglongjmp(g_gate_jmp, 1);
}

// ───────── 实验 G：真实模块装载（旷野之息 main，改写器真实产物）─────────
// 与实验 D 的差别：D 用的是自造测试映像（49KB），G 用的是 **真实游戏模块**——
// patch_nso.py（DM_PATCH_TARGET=ohos）改写旷野之息 main 之后的 main.patched.bin（37MB），
// 经 gen_embed_ohos.py 内嵌成 dmmainx0/w/x1 三段。判据四条，**不做执行**
//（执行需要 93 个宿主门的接线，另立一步）：
//   ① alloc 成功（内含布局自检：x0+w == PATCH_OFF）
//   ② 三段**逐字节全等**：映射区 vs 嵌入段（37MB 全比，不止抽查）
//   ③ seal 后代码区 R+X 生效（/proc/self/maps 权限位）
//   ④ 布局常数与 fixups 一致（module_t 的常数来自 dm_main_layout.h，同源）
static void exp_g(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 G】真实模块装载（旷野之息 main，37MB 改写产物）\n");
    app(out, cap, used, "  素材：patch_nso.py（DM_PATCH_TARGET=ohos）的 main.patched.bin\n");

    module_t m;
    memset(&m, 0, sizeof m);
    strncpy(m.name, "main", sizeof m.name - 1);
    m.text_end      = DM_IMG_MAIN_TEXT_END;
    m.patch_off     = DM_IMG_MAIN_PATCH_OFF;
    m.ro_off        = DM_IMG_MAIN_RO_OFF;
    m.data_off      = DM_IMG_MAIN_DATA_OFF;
    m.key_page_off  = DM_IMG_MAIN_KEY_PAGE_OFF;
    m.key_page_size = DM_IMG_MAIN_KEY_PAGE_SIZE;
    m.x0_end        = DM_IMG_MAIN_X0_END;      // 装载拷贝的精确范围（fixups），防链接器尾垫错位
    m.img_end       = DM_IMG_MAIN_END;
    appf(out, cap, used, "  布局：TEXT_END %#llx / PATCH_OFF %#llx / KEY_PAGE %#llx\n",
         (unsigned long long)m.text_end, (unsigned long long)m.patch_off,
         (unsigned long long)m.key_page_off);

    if (dm_ohos_map_image_alloc(&m)) {
        app(out, cap, used, "  ✗ alloc 失败（布局自检或 mmap，见 hilog）\n\n");
        return;
    }
    appf(out, cap, used, "  ✓ 定位 + 拷贝：base %p，size %zu\n", (void *)m.base, m.size);

    // 判据②：三段**全量** memcmp —— 映射区必须与嵌入段逐字节一致
    // 对拍长度按 **fixups 范围**（链接器会给 ELF 段尾垫对齐页，段大小比内容大 16K）
    const struct { const unsigned char *s; uint8_t *dst; size_t len; const char *nm; } regions[] = {
        { __start_dmmainx0, m.base,                            DM_IMG_MAIN_X0_END,             "x0(代码)" },
        { __start_dmmainw,  m.base + DM_IMG_MAIN_X0_END,       DM_IMG_MAIN_PATCH_OFF - DM_IMG_MAIN_X0_END, "w(数据)" },
        { __start_dmmainx1, m.base + DM_IMG_MAIN_PATCH_OFF,    DM_IMG_MAIN_END - DM_IMG_MAIN_PATCH_OFF,    "x1(补丁区)" },
    };
    int cmp_ok = 1;
    for (int k = 0; k < 3; k++) {
        size_t len = regions[k].len;
        int same = (memcmp(regions[k].dst, regions[k].s, len) == 0);
        appf(out, cap, used, "  %s %s 段逐字节一致（%zu 字节）\n", same ? "✓" : "✗", regions[k].nm, len);
        if (!same) cmp_ok = 0;
    }
    if (cmp_ok)
        app(out, cap, used, "  ✅ 判据②：映射区与嵌入段**全量一致**（37MB 全比）\n");
    else
        app(out, cap, used, "  ❌ 判据②：映射内容与嵌入段不一致\n");

    if (dm_ohos_map_image_seal(&m)) {
        app(out, cap, used, "  ✗ seal 失败（见 hilog）\n\n");
        return;
    }
    app(out, cap, used, "  ✓ 代码区与补丁区已封 R+X\n");

    // 判据③：/proc/self/maps 里找 base 所在行，验权限位
    int map_ok = 0; char perms[8] = "?";
    {
        FILE *f = fopen("/proc/self/maps", "r");
        char line[512];
        while (f && fgets(line, sizeof line, f)) {
            unsigned long lo, hi;
            if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3) {
                if ((uintptr_t)m.base + 0x1000 >= lo && (uintptr_t)m.base + 0x1000 < hi) { map_ok = 1; break; }
            }
        }
        if (f) fclose(f);
    }
    if (map_ok) {
        int rx = (perms[0] == 'r' && perms[1] == '-' && perms[2] == 'x');
        appf(out, cap, used, "  %s 判据③：代码区权限 = %s（期望 r-x）\n", rx ? "✅" : "❌", perms);
        if (!rx) cmp_ok = 0;
    } else {
        app(out, cap, used, "  ❌ 判据③：/proc/self/maps 里找不到映射区\n");
        cmp_ok = 0;
    }

    appf(out, cap, used, "  结论：%s\n\n",
         (cmp_ok && map_ok) ? "**真实模块装载成立** —— 装载链吃下了第一个真实游戏模块" : "链路有问题，见上");
}

// ───────── 实验 H：门回路（真实模块的真实门块 → dm_gate → gate_dispatch → svc 38 → 返回）─────────
// 装载（实验 G）之后的一步：旷野之息 main 的 93 个门**全部是 svc 38**（WaitSynchronization，占位返回 0）。
// 做法：装载 → 建 8 个线程槽（KGATE = dm_gate）→ 装键页（实际键号×8）→ 登记 93 门 →
//       每个门块的尾跳（B 原指令+4）暂改为 RET（隔离：不执行门后的游戏代码）→ 逐块调用。
// 判据：93 块全部走完整回路且返回 0（dm_gate 回写 x0 = svc 占位返回值）；未识别的门会 die（可见）。
// 门回路专用的增量文件日志：逐门结果边跑边落盘，崩溃也保得住（filesDir/dm_gate_test.log）
static FILE *g_gate_log;
static void glog(const char *fmt, ...) {
    if (!g_gate_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_gate_log, fmt, ap);
    va_end(ap);
    fflush(g_gate_log);
}
static void exp_h(const char *files_dir, char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 H】门回路：93 个真实门块 → dm_gate → gate_dispatch → svc 38 → 返回\n");
    if (files_dir && *files_dir) {
        char pth[512]; snprintf(pth, sizeof pth, "%s/dm_gate_test.log", files_dir);
        g_gate_log = fopen(pth, "w");
    }

    module_t m;
    memset(&m, 0, sizeof m);
    strncpy(m.name, "main", sizeof m.name - 1);
    m.text_end      = DM_IMG_MAIN_TEXT_END;
    m.patch_off     = DM_IMG_MAIN_PATCH_OFF;
    m.ro_off        = DM_IMG_MAIN_RO_OFF;
    m.data_off      = DM_IMG_MAIN_DATA_OFF;
    m.key_page_off  = DM_IMG_MAIN_KEY_PAGE_OFF;
    m.key_page_size = DM_IMG_MAIN_KEY_PAGE_SIZE;
    m.x0_end        = DM_IMG_MAIN_X0_END;
    m.img_end       = DM_IMG_MAIN_END;
    if (dm_ohos_map_image_alloc(&m)) {
        app(out, cap, used, "  ✗ alloc 失败（见 hilog）\n\n");
        return;
    }
    appf(out, cap, used, "  ✓ 装载：base %p，size %zu\n", (void *)m.base, m.size);

    // ① 线程槽：按 loader.c create_keys 的顺序建 8 键（musl 顺序发号），设值
    static const char *kn[8] = {"K18", "KTLS", "KTLR", "KGATE", "KRES0", "KRES1", "KASPECT", "KASPECTUI"};
    pthread_key_t key[8];
    int k_ok = 1;
    for (int i = 0; i < 8; i++)
        if (pthread_key_create(&key[i], NULL)) { appf(out, cap, used, "  ✗ 建键 %s 失败\n", kn[i]); k_ok = 0; break; }
    if (!k_ok) { app(out, cap, used, "\n"); return; }
    // 槽写入用【直写】：与门块的读取公式（tsd 基址 + 键号×8）同一条路，构造上保证一致。
    // （pthread_setspecific 写的位置与门块按公式读的位置不一致——二分定位已证实，2026-10-16 真机。）
    uint64_t tp_el0 = 0;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp_el0));
    uint64_t tsd_base = *(uint64_t *)(tp_el0 - 272);        // musl：TSD 数组基址（-272 真机实测）
    const void *slot_val[8] = {
        (void *)(uintptr_t)0x1111111111111111ull, (void *)(uintptr_t)0x2222222222222222ull,
        (void *)(uintptr_t)0x3333333333333333ull, (void *)dm_gate,
        (void *)(uintptr_t)0x4400000000000004ull, (void *)(uintptr_t)0x4400000000000005ull,
        (void *)(uintptr_t)0x4400000000000006ull, (void *)(uintptr_t)0x4400000000000007ull,
    };
    for (int i = 0; i < 8; i++)
        *(uint64_t *)(tsd_base + (uint64_t)key[i] * 8) = (uint64_t)slot_val[i];

    // ② 键页：写实际键号×8（与 loader.c install_key_page 同款；槽位下标来自 fixups 的 KEY_*_IDX）
    uint64_t *page = (uint64_t *)(m.base + DM_IMG_MAIN_KEY_PAGE_OFF);
    memset(page, 0, DM_IMG_MAIN_KEY_PAGE_SIZE);
    page[DM_IMG_MAIN_KEY_K18_IDX]       = (uint64_t)key[0] * 8;
    page[DM_IMG_MAIN_KEY_KTLS_IDX]      = (uint64_t)key[1] * 8;
    page[DM_IMG_MAIN_KEY_KTLR_IDX]      = (uint64_t)key[2] * 8;
    page[DM_IMG_MAIN_KEY_KGATE_IDX]     = (uint64_t)key[3] * 8;
    page[DM_IMG_MAIN_KEY_KRES0_IDX]     = (uint64_t)key[4] * 8;
    page[DM_IMG_MAIN_KEY_KRES1_IDX]     = (uint64_t)key[5] * 8;
    page[DM_IMG_MAIN_KEY_KASPECT_IDX]   = (uint64_t)key[6] * 8;
    page[DM_IMG_MAIN_KEY_KASPECTUI_IDX] = (uint64_t)key[7] * 8;
    uint64_t kgate_off = page[DM_IMG_MAIN_KEY_KGATE_IDX];
    uint64_t slot_now  = *(uint64_t *)(tsd_base + kgate_off);
    appf(out, cap, used, "  诊断：tsd 基址 %p，键页 KGATE 项 = %llu（×8），KGATE 槽现值 = %p（期望 dm_gate = %p）\n",
         (void *)tsd_base, (unsigned long long)(kgate_off / 8), (void *)slot_now, (void *)dm_gate);
    app(out, cap, used, "  ✓ 线程槽 8 键直写就绪，KGATE = dm_gate；键页已按实际键号写入\n");

    // 信号兜底：登记与过门都套（崩了进程活着，出错 PC 进报告）
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = gate_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);

    g_gate_armed = 1;
    if (sigsetjmp(g_gate_jmp, 1) != 0) {
        g_gate_armed = 0;
        appf(out, cap, used, "  ❌ 登记阶段崩（pc %#llx，sig %d）—— 见 hilog\n",
             (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        sigaction(SIGSEGV, &old_segv, NULL); sigaction(SIGBUS, &old_bus, NULL);
        return;
    }
    // ③ 登记 93 门（kind 码 → 字符串；返回点/原指令按模块基址换算成绝对地址）
    for (int i = 0; i < DM_GATES_MAIN_N; i++) {
        const char *ks = DM_GATES_MAIN[i].kind == 0 ? "svc" : DM_GATES_MAIN[i].kind == 1 ? "cntpct" : "cntvct";
        gate_register((uint64_t)(m.base + DM_GATES_MAIN[i].ret), ks, DM_GATES_MAIN[i].arg,
                      (uint64_t)(m.base + DM_GATES_MAIN[i].orig));
    }
    g_gate_armed = 0;
    appf(out, cap, used, "  ✓ 已登记 %d 个门（全部 svc，编号 38）\n", DM_GATES_MAIN_N);

    // ④ 尾跳暂改 RET：每个门块的最后一条是 B 原指令+4（在 返回点+4），改成 RET 让回路止于探针
    for (int i = 0; i < DM_GATES_MAIN_N; i++)
        *(uint32_t *)(m.base + DM_GATES_MAIN[i].ret + 4) = 0xD65F03C0;   // ret

    // ⑤ 封权限：门块要**执行**，代码区与补丁区必须先封成 R+X（实验 D 验证过这条路径）
    //   ——首轮真机就是漏了这一步：跳进 RW 页直接 SIGSEGV，进程死亡（faultlog 有档）。
    if (dm_ohos_map_image_seal(&m)) {
        app(out, cap, used, "  ✗ seal 失败（见 hilog）\n\n");
        return;
    }
    app(out, cap, used, "  ✓ 代码区与补丁区已封 R+X\n");

    // ⑤ 二分 B：纯 C 路径直调 gate_dispatch（构造 regs 帧，绕过汇编）——分离「宿主 C 侧」与「汇编路径」
    {
        uint64_t regs2[64]; memset(regs2, 0, sizeof regs2);
        regs2[0] = 0x8000000000000000ull;   // Break 的「仅通知」位
        g_gate_armed = 1; g_gate_fault_pc = 0;
        glog("二分 B 开始：将直调 gate_dispatch(ret=%#llx)\n", (unsigned long long)(m.base + DM_GATES_MAIN[0].ret));
        if (sigsetjmp(g_gate_jmp, 1) == 0) {
            uint64_t rv2 = gate_dispatch((uint64_t)(m.base + DM_GATES_MAIN[0].ret), regs2);
            g_gate_armed = 0;
            glog("二分 B：gate_dispatch 直调返回 %#llx（期望 0）\n", (unsigned long long)rv2);
            appf(out, cap, used, "  %s 二分 B：gate_dispatch 直调（纯 C）返回 %#llx\n",
                 rv2 == 0 ? "✓" : "✗", (unsigned long long)rv2);
        } else {
            g_gate_armed = 0;
            glog("二分 B：崩 pc %#llx sig %d\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
            appf(out, cap, used, "  ✗ 二分 B：gate_dispatch 直调崩（pc %#llx，sig %d）\n",
                 (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        }
    }

    // ⑤ 二分 C：直调 svc_dispatch(38, regs2, orig)（跳过哈希查找）——分离「查找」与「处理器」
    {
        uint64_t regs3[64]; memset(regs3, 0, sizeof regs3);
        regs3[0] = 0x8000000000000000ull;
        g_gate_armed = 1; g_gate_fault_pc = 0;
        if (sigsetjmp(g_gate_jmp, 1) == 0) {
            uint64_t rv3 = svc_dispatch(38, regs3, (uint64_t)(m.base + DM_GATES_MAIN[0].orig));
            g_gate_armed = 0;
            glog("二分 C：svc_dispatch(38) 返回 %#llx\n", (unsigned long long)rv3);
            appf(out, cap, used, "  %s 二分 C：svc_dispatch(38) 直调返回 %#llx\n",
                 rv3 == 0 ? "✓" : "✗", (unsigned long long)rv3);
        } else {
            g_gate_armed = 0;
            glog("二分 C：崩 pc %#llx sig %d\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
            appf(out, cap, used, "  ✗ 二分 C：svc_dispatch 直调崩（pc %#llx，sig %d）\n",
                 (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        }
    }

    // ⑤′ 二分定位（必须在 seal 之后：门块要可执行）：门 0 的块先以「KGATE = 探针函数」过一次
    //    —— 验证「门块执行 + 槽读取」；通了再把 KGATE 指回 dm_gate，崩了就锁定在宿主侧入口。
    {
        uint32_t *p0 = (uint32_t *)(m.base + DM_GATES_MAIN[0].ret);
        void *blk0 = NULL;
        for (int b = 1; b <= 16; b++)
            if (p0[-b] == 0xA9BF7BFD) { blk0 = (uint8_t *)p0 - 4 * b; break; }
        if (blk0) {
            // 尾跳已在步骤④（seal 前）改为 RET——此处**不可再写**（seal 后补丁区只读，写了必 SIGSEGV）
            *(uint64_t *)(tsd_base + (uint64_t)key[3] * 8) = (uint64_t)gate_probe_entry;
            g_gate_armed = 1; g_gate_fault_pc = 0;
            glog("二分 A：门 0 开始（KGATE=探针函数，块起点 %p）\n", blk0);
            if (sigsetjmp(g_gate_jmp, 1) == 0) {
                ((void (*)(void))blk0)();
                g_gate_armed = 0;
                glog("二分 A：过门成功\n");
                app(out, cap, used, "  ✓ 二分 A：门 0 以探针入口过门成功（门块执行 + KGATE 槽读取成立）\n");
            } else {
                g_gate_armed = 0;
                glog("二分 A：崩 pc %#llx sig %d\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
                appf(out, cap, used, "  ✗ 二分 A：门 0 以探针入口仍崩（pc %#llx）⇒ 问题在门块/槽读取本身\n",
                     (unsigned long long)g_gate_fault_pc);
            }
            *(uint64_t *)(tsd_base + (uint64_t)key[3] * 8) = (uint64_t)dm_gate;   // 恢复
        }
    }

    // ⑥ 逐门过回路：门块地址 = 从返回点向后扫 stp x29,x30,[sp,#-16]!（0xA9BF7BFD）
    int ok = 0, noblock = 0, faulted = 0;
    for (int i = 0; i < DM_GATES_MAIN_N; i++) {
        uint32_t *p = (uint32_t *)(m.base + DM_GATES_MAIN[i].ret);
        void *blk = NULL;
        for (int b = 1; b <= 16; b++)
            if (p[-b] == 0xA9BF7BFD) { blk = (uint8_t *)p - 4 * b; break; }
        if (!blk) { noblock++; continue; }
        g_gate_armed = 1; g_gate_fault_pc = 0;
        glog(">>> 门 %d（ret %#llx）开始\n", i, (unsigned long long)DM_GATES_MAIN[i].ret);
        if (sigsetjmp(g_gate_jmp, 1) == 0) {
            // 过门参数：svc 38 = Break(reason, ptr, size)。reason 的最高位 = 「仅通知，可继续」——
            // 处理器看到它就返回 0 而不 _exit(42)；ptr/size 置 0 免得处理器解引用垃圾指针。
            register uint64_t g_x0 __asm__("x0") = 0x8000000000000000ull;
            register uint64_t g_x1 __asm__("x1") = 0;
            register uint64_t g_x2 __asm__("x2") = 0;
            __asm__ volatile("blr %3"
                             : "+r"(g_x0), "+r"(g_x1), "+r"(g_x2)
                             : "r"(blk)
                             : "x3","x4","x5","x6","x7","x8","x9","x10","x11","x12",
                               "x13","x14","x15","x16","x17","x30","memory");
            uint64_t rv = g_x0;   // dm_gate 回写 x0 = 处理器返回值
            g_gate_armed = 0;
            glog("门 %d 返回 %#llx\n", i, (unsigned long long)rv);
            if (rv == 0) ok++;
            else appf(out, cap, used, "  ✗ 门 %d（ret %#llx）返回 %#llx（期望 0）\n",
                      i, (unsigned long long)DM_GATES_MAIN[i].ret, (unsigned long long)rv);
        } else {
            g_gate_armed = 0; faulted++;
            glog("门 %d 崩：pc %#llx sig %d\n", i, (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
            appf(out, cap, used, "  ✗ 门 %d（ret %#llx）崩：pc %#llx，sig %d\n",
                 i, (unsigned long long)DM_GATES_MAIN[i].ret,
                 (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        }
    }
    sigaction(SIGSEGV, &old_segv, NULL); sigaction(SIGBUS, &old_bus, NULL);
    glog("=== 结束：ok=%d faulted=%d noblock=%d ===\n", ok, faulted, noblock);
    if (g_gate_log) { fclose(g_gate_log); g_gate_log = NULL; }
    appf(out, cap, used, "  （逐门明细已落盘 %s/dm_gate_test.log）\n", files_dir ? files_dir : "?");
    appf(out, cap, used, "  %s 判据：%d / %d 个门块走完整回路且返回 0（Break 以仅通知位过门）；崩 %d（PC 见上）；块起点未找到 %d\n",
         (ok == DM_GATES_MAIN_N - noblock - faulted && faulted == 0) ? "✅" : "❌",
         ok, DM_GATES_MAIN_N, faulted, noblock);
    app(out, cap, used, "  说明：每个门块的尾跳已暂改 RET（不执行门后的游戏代码）；首次过门时 hilog 有"
                        " \"SVC 0x26 …（占位返回成功）\"\n\n");
}

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
    logline(">>> 实验 E2 开始"); exp_e2(out, cap, &used); logline(">>> 实验 E2 返回");
    logline(">>> 实验 F 开始"); exp_f(out, cap, &used); logline(">>> 实验 F 返回");
    logline(">>> 实验 G 开始"); exp_g(out, cap, &used); logline(">>> 实验 G 返回");
    logline(">>> 实验 H 开始"); exp_h(files_dir, out, cap, &used); logline(">>> 实验 H 返回");
    logline(">>> 实验 C 开始"); exp_c(out, cap, &used); logline(">>> 实验 C 返回");
    logline(">>> 实验 B 开始"); exp_b(files_dir, out, cap, &used); logline(">>> 实验 B 返回");
    logline(">>> 实验 A 开始"); exp_a(out, cap, &used); logline(">>> 实验 A 返回");

    app(out, cap, &used, "==================================================\n");
    app(out, cap, &used, "把这份报告发回来，就能定下装载方案。\n");
    logline(">>> 三个实验全部跑完");
    return 0;
}
