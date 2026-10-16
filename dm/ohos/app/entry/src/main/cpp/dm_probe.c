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
        if (p < 0x10000 || p > 0x80000000ull) continue;    // 不像用户态指针
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
// 看门狗假设验证：二分 B 的门调度搬到专用线程上跑（槽在该线程重设）——与上游设计一致
// （上游门调用发生在客体线程，不在主线程）。主线程卡死保护可能是此前"无痕死亡"的真因。
typedef struct { uint64_t base; pthread_key_t key[8]; uint64_t rv; int faulted; } gb_ctx_t;
static gb_ctx_t g_gb;
static void *gb_thread(void *arg) {
    gb_ctx_t *c = arg;
    uint64_t tp = 0; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));      // 本线程的 tsd
    uint64_t tsd = *(uint64_t *)(tp - 272);
    const void *sv[8] = {
        (void *)(uintptr_t)0x1111111111111111ull, (void *)(uintptr_t)0x2222222222222222ull,
        (void *)(uintptr_t)0x3333333333333333ull, (void *)dm_gate,
        (void *)(uintptr_t)0x4400000000000004ull, (void *)(uintptr_t)0x4400000000000005ull,
        (void *)(uintptr_t)0x4400000000000006ull, (void *)(uintptr_t)0x4400000000000007ull,
    };
    for (int i = 0; i < 8; i++) *(uint64_t *)(tsd + (uint64_t)c->key[i] * 8) = (uint64_t)sv[i];
    uint64_t regs2[64]; memset(regs2, 0, sizeof regs2);
    regs2[0] = 0x80000000ull;
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        g_gb.rv = gate_dispatch(c->base, regs2);
        g_gate_armed = 0;
    } else { g_gate_armed = 0; c->faulted = 1; }
    return NULL;
}

// 实验 I：真实执行——调用模块里的 3 个真实小函数（离线反汇编选出的纯叶子函数，
// 行为完全可预测），在专用线程上调用并核对返回值。这是执行链从"门回路验证"走向
// "客体代码真正跑起来"的第一站。
static uint64_t g_i_rv[3];
static module_t g_i_m;
static volatile int g_i_faulted;
static void *i_thread(void *arg) {
    (void)arg;
    uint64_t b = (uint64_t)g_i_m.base;
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        g_i_rv[0] = ((uint64_t (*)(uint64_t))(b + 0x2eb80))(0x1000);   // add x0,x0,#0x14; ret
        g_i_rv[1] = ((uint64_t (*)(uint64_t))(b + 0x2f3ad4))(0x5000);  // add x0,x0,#0x80; ret
        g_i_rv[2] = ((uint64_t (*)(void))(b + 0x157a10))();            // mov w0,#-1; ret
        g_gate_armed = 0;
    } else {
        g_gate_armed = 0; g_i_faulted = 1;
        g_i_rv[0] = g_i_rv[1] = g_i_rv[2] = ~0ull;
    }
    return NULL;
}
static void exp_i(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 I】真实执行：调用模块里 3 个真实函数并核对返回值\n");
    memset(&g_i_m, 0, sizeof g_i_m);
    strncpy(g_i_m.name, "main", sizeof g_i_m.name - 1);
    g_i_m.text_end      = DM_IMG_MAIN_TEXT_END;
    g_i_m.patch_off     = DM_IMG_MAIN_PATCH_OFF;
    g_i_m.ro_off        = DM_IMG_MAIN_RO_OFF;
    g_i_m.data_off      = DM_IMG_MAIN_DATA_OFF;
    g_i_m.key_page_off  = DM_IMG_MAIN_KEY_PAGE_OFF;
    g_i_m.key_page_size = DM_IMG_MAIN_KEY_PAGE_SIZE;
    g_i_m.x0_end        = DM_IMG_MAIN_X0_END;
    g_i_m.img_end       = DM_IMG_MAIN_END;
    if (dm_ohos_map_image_alloc(&g_i_m)) { app(out, cap, used, "  ✗ alloc 失败\n\n"); return; }
    if (dm_ohos_map_image_seal(&g_i_m)) { app(out, cap, used, "  ✗ seal 失败\n\n"); return; }
    appf(out, cap, used, "  ✓ 装载并封 R+X：base %p\n", (void *)g_i_m.base);

    struct sigaction sa, o1, o2, o3, o4;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = gate_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1); sigaction(SIGBUS, &sa, &o2);
    sigaction(SIGILL, &sa, &o3);  sigaction(SIGTRAP, &sa, &o4);

    g_i_faulted = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, i_thread, NULL) == 0) pthread_join(th, NULL);
    else app(out, cap, used, "  ✗ pthread_create 失败\n");

    sigaction(SIGSEGV, &o1, NULL); sigaction(SIGBUS, &o2, NULL);
    sigaction(SIGILL, &o3, NULL);  sigaction(SIGTRAP, &o4, NULL);

    if (g_i_faulted) {
        appf(out, cap, used, "  ✗ 执行崩（pc %#llx sig %d）\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        app(out, cap, used, "\n");
        return;
    }
    int ok = 0;
    appf(out, cap, used, "  %s f₁(0x1000)=%#llx（期望 0x1014）  f₁: add x0,x0,#0x14; ret\n",
         g_i_rv[0] == 0x1014 ? "✓" : "✗", (unsigned long long)g_i_rv[0]);
    if (g_i_rv[0] == 0x1014) ok++;
    appf(out, cap, used, "  %s f₂(0x5000)=%#llx（期望 0x5080）  f₂: add x0,x0,#0x80; ret\n",
         g_i_rv[1] == 0x5080 ? "✓" : "✗", (unsigned long long)g_i_rv[1]);
    if (g_i_rv[1] == 0x5080) ok++;
    uint64_t r2 = g_i_rv[2] & 0xffffffffull;
    appf(out, cap, used, "  %s f₃()=%#llx（期望 w0=0xffffffff）  f₃: mov w0,#-1; ret\n",
         r2 == 0xffffffffull ? "✓" : "✗", (unsigned long long)g_i_rv[2]);
    if (r2 == 0xffffffffull) ok++;
    appf(out, cap, used, "  %s 判据：%d / 3 个真实函数在宿主执行且返回值精确正确\n",
         ok == 3 ? "✅" : "❌", ok);
    app(out, cap, used, "  意义：客体代码可以在鸿蒙宿主上真正执行并正确返回——执行链地基成立。\n");
    app(out, cap, used, "\n");
}

// 实验 J：真实函数 + 真实门 串成一条链——调用含门的真实函数，命中门后由**我们的服务**应答，
// 并验证服务的返回值从真实函数的出口流回来。这是执行链从"能跑"走向"跑真实流程"的一站。
// 目标函数 0x140c9d8（28B，零调用零全局寻址，含门@0x140c9ec）：
//   cmp w2,#1 ; b.lt → 门 ; cmp w1,w2 ; b.gt → 门 ; b 0x1632d40（正常尾调）
//   门分支：命中门 → 返回值落在 x0 → 0x140c9f0: ret  ⇒ 函数返回门的返回值
static volatile int g_j_hits;
static uint64_t g_j_rv;
static volatile int g_j_faulted;
static module_t g_j_m;
static pthread_key_t g_j_key[8];
static uint64_t j_gate_stub(void) { g_j_hits++; return 0xABCD; }   // 我们的"服务"：记命中、返回已知值
static void *j_thread(void *arg) {
    (void)arg;
    uint64_t b = (uint64_t)g_j_m.base;
    uint64_t tp = 0; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t tsd = *(uint64_t *)(tp - 272);
    const void *sv[8] = {
        (void *)(uintptr_t)0x1111111111111111ull, (void *)(uintptr_t)0x2222222222222222ull,
        (void *)(uintptr_t)0x3333333333333333ull, (void *)j_gate_stub,   // KGATE = 我们的服务
        (void *)(uintptr_t)0x4400000000000004ull, (void *)(uintptr_t)0x4400000000000005ull,
        (void *)(uintptr_t)0x4400000000000006ull, (void *)(uintptr_t)0x4400000000000007ull,
    };
    for (int i = 0; i < 8; i++) *(uint64_t *)(tsd + (uint64_t)g_j_key[i] * 8) = (uint64_t)sv[i];
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        // w2=0 ⇒ 走门分支（cmp w2,#1 → b.lt）
        g_j_rv = ((uint64_t (*)(uint64_t, uint64_t, uint64_t))(b + 0x140c9d8))(0, 0, 0);
        g_gate_armed = 0;
    } else { g_gate_armed = 0; g_j_faulted = 1; }
    return NULL;
}
static void exp_j(char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 J】真实函数 + 真实门 串链：服务返回值从函数出口流出\n");
    memset(&g_j_m, 0, sizeof g_j_m);
    strncpy(g_j_m.name, "main", sizeof g_j_m.name - 1);
    g_j_m.text_end      = DM_IMG_MAIN_TEXT_END;
    g_j_m.patch_off     = DM_IMG_MAIN_PATCH_OFF;
    g_j_m.ro_off        = DM_IMG_MAIN_RO_OFF;
    g_j_m.data_off      = DM_IMG_MAIN_DATA_OFF;
    g_j_m.key_page_off  = DM_IMG_MAIN_KEY_PAGE_OFF;
    g_j_m.key_page_size = DM_IMG_MAIN_KEY_PAGE_SIZE;
    g_j_m.x0_end        = DM_IMG_MAIN_X0_END;
    g_j_m.img_end       = DM_IMG_MAIN_END;
    if (dm_ohos_map_image_alloc(&g_j_m)) { app(out, cap, used, "  ✗ alloc 失败\n\n"); return; }
    if (dm_ohos_map_image_seal(&g_j_m)) { app(out, cap, used, "  ✗ seal 失败\n\n"); return; }
    appf(out, cap, used, "  ✓ 装载并封 R+X：base %p\n", (void *)g_j_m.base);

    for (int i = 0; i < 8; i++)
        if (pthread_key_create(&g_j_key[i], NULL)) { app(out, cap, used, "  ✗ 建键失败\n\n"); return; }
    // 键页按实际键号写入（门块照此寻址）
    uint64_t *page = (uint64_t *)(g_j_m.base + DM_IMG_MAIN_KEY_PAGE_OFF);
    memset(page, 0, DM_IMG_MAIN_KEY_PAGE_SIZE);
    page[DM_IMG_MAIN_KEY_K18_IDX]       = (uint64_t)g_j_key[0] * 8;
    page[DM_IMG_MAIN_KEY_KTLS_IDX]      = (uint64_t)g_j_key[1] * 8;
    page[DM_IMG_MAIN_KEY_KTLR_IDX]      = (uint64_t)g_j_key[2] * 8;
    page[DM_IMG_MAIN_KEY_KGATE_IDX]     = (uint64_t)g_j_key[3] * 8;
    page[DM_IMG_MAIN_KEY_KRES0_IDX]     = (uint64_t)g_j_key[4] * 8;
    page[DM_IMG_MAIN_KEY_KRES1_IDX]     = (uint64_t)g_j_key[5] * 8;
    page[DM_IMG_MAIN_KEY_KASPECT_IDX]   = (uint64_t)g_j_key[6] * 8;
    page[DM_IMG_MAIN_KEY_KASPECTUI_IDX] = (uint64_t)g_j_key[7] * 8;

    struct sigaction sa, o1, o2, o3, o4;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = gate_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1); sigaction(SIGBUS, &sa, &o2);
    sigaction(SIGILL, &sa, &o3);  sigaction(SIGTRAP, &sa, &o4);

    g_j_hits = 0; g_j_rv = 0; g_j_faulted = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, j_thread, NULL) == 0) pthread_join(th, NULL);
    else app(out, cap, used, "  ✗ pthread_create 失败\n");

    sigaction(SIGSEGV, &o1, NULL); sigaction(SIGBUS, &o2, NULL);
    sigaction(SIGILL, &o3, NULL);  sigaction(SIGTRAP, &o4, NULL);

    if (g_j_faulted) {
        appf(out, cap, used, "  ✗ 执行崩（pc %#llx sig %d）\n\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        return;
    }
    appf(out, cap, used, "  %s 门被真实函数命中：%d 次（期望 1）\n", g_j_hits == 1 ? "✓" : "✗", g_j_hits);
    appf(out, cap, used, "  %s 服务返回值从函数出口流出：%#llx（期望 0xabcd）\n",
         g_j_rv == 0xABCD ? "✓" : "✗", (unsigned long long)g_j_rv);
    appf(out, cap, used, "  %s 判据：真实函数 → 命中门 → 我们的服务应答 → 值流回函数出口\n",
         (g_j_hits == 1 && g_j_rv == 0xABCD) ? "✅" : "❌");
    app(out, cap, used, "  意义：门与执行串成一条链——真实代码可以借我们的服务跑通一条完整路径。\n\n");
}

// 实验 K：真实流程首跑——从模块的启动链（映像里第一个函数 0x1c8）起跑，跑到哪里出事就记哪里。
// KGATE 槽指向记录型服务（记命中与返回点），所有日志落盘（崩溃也保得住）。
static FILE *g_k_log;
static void klog(const char *fmt, ...) {
    if (!g_k_log) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_k_log, fmt, ap);
    va_end(ap);
    fflush(g_k_log);
}
static volatile int g_k_hits;
static uint64_t g_k_hit_lr[16];
static volatile int g_k_faulted;
static uint64_t g_k_rv;
static module_t g_k_m;
static pthread_key_t g_k_key[8];
static uint64_t k_gate_stub(void) {
    uint64_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
    if (g_k_hits < 16) g_k_hit_lr[g_k_hits] = lr;
    g_k_hits++;
    return 0;   // 首跑：先一律"无事发生"（0），看流程能走多远
}
// 记录型 PLT 存根：x16 = 该槽地址（thunk 里 add x16,x16,#imm 后 br），据此认出是哪个 SDK 符号。
typedef struct { uint64_t slot; const char *name; } plt_ent_t;
static plt_ent_t g_plt[512];
static int g_nplt;
static volatile int g_plt_calls;
// 宿主侧"真服务"表：启动链真的需要返回值的几个（不是一律返 0）。
// 依据：崩点反汇编显示 AllocateMemoryBlock 的返回值被当对象指针使用。
static char g_host_arg0[] = "main";
static char *g_host_argv[2];
static uint64_t plt_stub(void) {
    uint64_t x16v, a0, a1, a2;
    __asm__ volatile("mov %0, x16" : "=r"(x16v));
    __asm__ volatile("mov %0, x0" : "=r"(a0));
    __asm__ volatile("mov %0, x1" : "=r"(a1));
    __asm__ volatile("mov %0, x2" : "=r"(a2));
    const char *nm = NULL;
    for (int i = 0; i < g_nplt; i++) if (g_plt[i].slot == x16v) { nm = g_plt[i].name; break; }
    g_plt_calls++;
    if (nm) {
        // nn::os::AllocateMemoryBlock(size_t* out_size, size_t size) —— 真给内存
        if (!strcmp(nm, "_ZN2nn2os19AllocateMemoryBlockEPmm")) {
            void *p = malloc((size_t)a1);
            if (a0) *(uint64_t *)a0 = a1;
            klog("[服务] AllocateMemoryBlock(大小 %llu) → %p\n", (unsigned long long)a1, p);
            return (uint64_t)(uintptr_t)p;
        }
        // nn::os::GetHostArgc / GetHostArgv —— 给一对合理的启动参数
        if (!strcmp(nm, "_ZN2nn2os11GetHostArgcEv")) { klog("[服务] GetHostArgc → 1\n"); return 1; }
        if (!strcmp(nm, "_ZN2nn2os11GetHostArgvEv")) {
            g_host_argv[0] = g_host_arg0; g_host_argv[1] = NULL;
            klog("[服务] GetHostArgv → argv[0]=main\n");
            return (uint64_t)(uintptr_t)g_host_argv;
        }
        klog("[PLT] %s\n", nm);
    }
    return 0;
}
// 宿主分配器：按游戏的对象布局喂堆。分配器 0x9f34d8 的路径：
//   *(0x226f0a0) → 持有者 → *持有者 = 「对象 A」→ (x1==0 时) 0xa0e2f8 取「当前分配器」
//   → [A+8] → *该对象 = 虚表 → 虚表[0x30] = 分配方法 → 虚调用 (x0=对象, x1=大小, w2=8)
// 所以造：A = { &虚表, &A }（+8 自指），虚表[6] = host_alloc；两个全局指向对应持有者。
static uint64_t g_alloc_calls;
static void *host_alloc(void *self, uint64_t size, uint64_t flags) {
    (void)self; (void)flags;
    g_alloc_calls++;
    void *p = malloc((size_t)size);
    klog("[堆] 分配 %llu 字节 → %p\n", (unsigned long long)size, p);
    return p;
}
static uint64_t g_alloc_vtbl[8];      // 虚表：槽 6（+0x30）= host_alloc
static uint64_t g_alloc_obj[2];       // 对象 A：{ &虚表, &A }
static uint64_t g_alloc_holder[1];    // 持有者：{ &A }
static uint64_t g_ctx_holder[1];      // 上下文持有者：{ &ctx }
static uint64_t g_ctx_obj[0x20];      // 上下文对象（[0x88] 处为 0 = 让 thunk 返回 0，走本线程路径）
static uint64_t g_mgr_obj[8];       // 管理器对象：+0x18 处放"子对象"指针
static uint64_t g_mgr_sub[8];       // 子对象：+0x18 处放虚方法指针
static uint64_t mgr_method(void *self) {
    klog("[管理器] 虚方法被调用（self=%p）—— 宿主存根应答\n", self);
    return 0;
}
// 日志对象 + 节点池：nnMain 开头调用的日志辅助 0xf35374 从 *(0x2288548) 取对象，
// 字段：[+0x10]=计数（0）、[+0x28]=上限（64）、[+0x18]=节点链表头。
// 节点：>=0x20 字节，[+0]=下一节点。链表**必须非空**（空表会让它往地址 0 写）。
#define LOG_POOL_N 64
static uint64_t g_log_pool[LOG_POOL_N][4];      // 每节点 0x20 字节
static uint64_t g_log_obj[8];
static void k_inject_log(uint8_t *img) {
    for (int i = 0; i < LOG_POOL_N; i++)
        g_log_pool[i][0] = (i + 1 < LOG_POOL_N) ? (uint64_t)(uintptr_t)&g_log_pool[i + 1][0] : 0;
    memset(g_log_obj, 0, sizeof g_log_obj);
    g_log_obj[0x10 / 8] = 0;                                              // 计数
    g_log_obj[0x28 / 8] = LOG_POOL_N;                                     // 上限
    g_log_obj[0x18 / 8] = (uint64_t)(uintptr_t)&g_log_pool[0][0];         // 链表头（非空）
    *(uint64_t *)(img + 0x2288548) = (uint64_t)(uintptr_t)&g_log_obj[0];
}
static void k_inject_heap(uint8_t *img) {
    g_alloc_vtbl[6] = (uint64_t)(uintptr_t)&host_alloc;
    g_alloc_obj[0] = (uint64_t)(uintptr_t)&g_alloc_vtbl[0];
    g_alloc_obj[1] = (uint64_t)(uintptr_t)&g_alloc_obj[0];   // +8 自指：0xa0e2f8 的兜底路径取到同一对象
    g_alloc_holder[0] = (uint64_t)(uintptr_t)&g_alloc_obj[0];
    g_ctx_holder[0] = (uint64_t)(uintptr_t)&g_ctx_obj[0];
    *(uint64_t *)(img + 0x226f0a0) = (uint64_t)(uintptr_t)&g_alloc_holder[0];   // 分配器持有者
    *(uint64_t *)(img + 0x22783d8) = (uint64_t)(uintptr_t)&g_ctx_holder[0];     // 线程上下文持有者
    // 工厂 0x1f8 的入参：读 [x1+0x18] → 对象 → [+0x18] → 虚方法（见崩点 0x103b8a8）
    g_mgr_sub[0x18 / 8] = (uint64_t)(uintptr_t)&mgr_method;
    g_mgr_obj[0x18 / 8] = (uint64_t)(uintptr_t)&g_mgr_sub[0];
}

static void *k_thread(void *arg) {
    (void)arg;
    uint64_t b = (uint64_t)g_k_m.base;
    uint64_t tp = 0; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uint64_t tsd = *(uint64_t *)(tp - 272);
    const void *sv[8] = {
        (void *)(uintptr_t)0x1111111111111111ull, (void *)(uintptr_t)0x2222222222222222ull,
        (void *)(uintptr_t)0x3333333333333333ull, (void *)k_gate_stub,
        (void *)(uintptr_t)0x4400000000000004ull, (void *)(uintptr_t)0x4400000000000005ull,
        (void *)(uintptr_t)0x4400000000000006ull, (void *)(uintptr_t)0x4400000000000007ull,
    };
    for (int i = 0; i < 8; i++) *(uint64_t *)(tsd + (uint64_t)g_k_key[i] * 8) = (uint64_t)sv[i];
    {   // 崩点归属用：把本进程映射抄进日志（模块区 / 探针 .so / 宿主堆 各自范围）
        FILE *mp = fopen("/proc/self/maps", "r");
        if (mp) {
            char line[256]; int n = 0;
            klog("=== 进程映射（前 24 行）===\n");
            while (n++ < 24 && fgets(line, sizeof line, mp)) klog("  %s", line);
            fclose(mp);
        }
    }
    klog("=== 起跑：目录 0x1c8（启动链）base %#llx ===\n", (unsigned long long)b);
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        g_k_rv = ((uint64_t (*)(void))(b + 0x1c8))();
        g_gate_armed = 0;
        klog("=== 启动链返回：rv=%#llx ===\n", (unsigned long long)g_k_rv);
    } else {
        g_gate_armed = 0; g_k_faulted = 1;
        klog("=== 启动链崩：pc %#llx sig %d ===\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        return NULL;
    }
    // 更深一站：模块的第一个真实功能函数 0x1f8（C++ 对象工厂：读数据槽 → 分配 0xaa0 → 构造）
    klog("=== 深跑：0x1f8（对象工厂，x0=0）===\n");
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        uint64_t r2 = ((uint64_t (*)(uint64_t))(b + 0x1f8))((uint64_t)(uintptr_t)&g_mgr_obj[0]);
        g_gate_armed = 0;
        klog("=== 0x1f8 返回：rv=%#llx ===\n", (unsigned long long)r2);
    } else {
        g_gate_armed = 0; g_k_faulted = 1;
        klog("=== 0x1f8 崩：pc %#llx sig %d ===\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
    }
    // 终站：游戏的真正入口 nnMain（模块符号表 #506：st_value 0x735330，size 1008，GLOBAL FUNC）
    // 依据：rtld 的符号表里列出它要为主模块解析的入口名 nnMain（启动约定，非猜测）。
    klog("=== 终站：nnMain（0x735330）===\n");
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        // 按"干净调用"准备寄存器：x21 = 模块基址（Switch SDK 约定），x0..x7 清零。
        register uint64_t r21 __asm__("x21") = b;
        register uint64_t r_x2 __asm__("x2") = 0; (void)r_x2;
        register uint64_t r_x3 __asm__("x3") = 0; (void)r_x3;
        register uint64_t r_x4 __asm__("x4") = 0; (void)r_x4;
        register uint64_t r_x5 __asm__("x5") = 0; (void)r_x5;
        register uint64_t r_x6 __asm__("x6") = 0; (void)r_x6;
        register uint64_t r_x7 __asm__("x7") = 0; (void)r_x7;
        __asm__ volatile("" : "+r"(r21), "+r"(r_x2), "+r"(r_x3), "+r"(r_x4), "+r"(r_x5), "+r"(r_x6), "+r"(r_x7));
        uint64_t r3 = ((uint64_t (*)(uint64_t, uint64_t))(b + 0x735330))(0, 0);
        g_gate_armed = 0;
        klog("=== nnMain 返回：rv=%#llx ===\n", (unsigned long long)r3);
    } else {
        g_gate_armed = 0; g_k_faulted = 1;
        klog("=== nnMain 崩：pc %#llx sig %d ===\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
    }
    return NULL;
}
static void exp_k(const char *files_dir, char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 K】真实流程首跑：从模块启动链（0x1c8）起跑\n");
    if (files_dir && *files_dir) {
        char pth[512]; snprintf(pth, sizeof pth, "%s/dm_run_test.log", files_dir);
        g_k_log = fopen(pth, "w");
        dm_log_set_mirror(g_k_log);
    }
    memset(&g_k_m, 0, sizeof g_k_m);
    strncpy(g_k_m.name, "main", sizeof g_k_m.name - 1);
    g_k_m.text_end      = DM_IMG_MAIN_TEXT_END;
    g_k_m.patch_off     = DM_IMG_MAIN_PATCH_OFF;
    g_k_m.ro_off        = DM_IMG_MAIN_RO_OFF;
    g_k_m.data_off      = DM_IMG_MAIN_DATA_OFF;
    g_k_m.key_page_off  = DM_IMG_MAIN_KEY_PAGE_OFF;
    g_k_m.key_page_size = DM_IMG_MAIN_KEY_PAGE_SIZE;
    g_k_m.x0_end        = DM_IMG_MAIN_X0_END;
    g_k_m.img_end       = DM_IMG_MAIN_END;
    if (dm_ohos_map_image_alloc(&g_k_m)) { app(out, cap, used, "  ✗ alloc 失败\n\n"); return; }

    // ===== 两步：① 应用 RELATIVE 重定位；② 392 个 PLT 槽填「记录型存根」 =====
    // 为什么：映像只是"照抄图纸"，加载器该做的重定位没做——数据段里的指针全是原始值，
    // GOT 槽还是「懒绑定占位」（存根自身地址）⇒ 第一个 SDK 调用就跳到沟里（实验 K 首跑实证）。
    {
        uint8_t *img = (uint8_t *)g_k_m.base;
        uint32_t mod0 = 0;
        for (uint32_t o = 0; o < 0x100; o += 4)
            if (*(uint32_t *)(img + o) == 0x30444F4Du) { mod0 = o; break; }   // 'MOD0'
        uint64_t dyn = (uint64_t)(img + mod0 + (mod0 ? *(uint32_t *)(img + mod0 + 4) : 0));   // 动态段偏移是相对 MOD0 起点
        uint64_t DT_RELA = 0, DT_RELASZ = 0, DT_RELACOUNT = 0;
        uint64_t DT_JMPREL = 0, DT_PLTRELSZ = 0, DT_SYMTAB = 0, DT_STRTAB = 0;
        for (int i = 0; mod0 && i < 64; i++) {
            uint64_t tag = *(uint64_t *)(dyn + (uint64_t)i * 16);
            uint64_t val = *(uint64_t *)(dyn + (uint64_t)i * 16 + 8);
            if (tag == 0) break;
            if (tag == 7) DT_RELA = val;
            else if (tag == 8) DT_RELASZ = val;
            else if (tag == 0x6ffffff9ull) DT_RELACOUNT = val;
            else if (tag == 0x17) DT_JMPREL = val;
            else if (tag == 2) DT_PLTRELSZ = val;
            else if (tag == 6) DT_SYMTAB = val;
            else if (tag == 5) DT_STRTAB = val;
        }
        uint64_t n_rel = 0, n_other = 0;
        for (uint64_t off = 0; off + 24 <= DT_RELASZ; off += 24) {
            uint64_t r_off  = *(uint64_t *)(img + DT_RELA + off);
            uint64_t r_info = *(uint64_t *)(img + DT_RELA + off + 8);
            int64_t  r_add  = *(int64_t  *)(img + DT_RELA + off + 16);
            if ((uint32_t)r_info == 1027) { *(uint64_t *)(img + r_off) = (uint64_t)img + (uint64_t)r_add; n_rel++; }
            else n_other++;
        }
        k_inject_heap(img);   // ★ 喂堆：把游戏分配器要的两个全局指向宿主对象（见 k_inject_heap）
    k_inject_log(img);    // ★ 喂日志对象 + 节点池（见 k_inject_log）
        g_nplt = 0; g_plt_calls = 0;
        for (uint64_t off = 0; off + 24 <= DT_PLTRELSZ; off += 24) {
            uint64_t r_off  = *(uint64_t *)(img + DT_JMPREL + off);
            uint64_t r_info = *(uint64_t *)(img + DT_JMPREL + off + 8);
            uint32_t symi   = (uint32_t)(r_info >> 32);
            const char *nm  = (const char *)(img + DT_STRTAB + *(uint32_t *)(img + DT_SYMTAB + (uint64_t)symi * 24));
            if (g_nplt < 512) { g_plt[g_nplt].slot = (uint64_t)(img + r_off); g_plt[g_nplt].name = nm; g_nplt++; }
            *(uint64_t *)(img + r_off) = (uint64_t)(uintptr_t)&plt_stub;   // 槽 → 记录型存根
        }
        klog("[重定位诊断] mod0=%#x dyn=%#llx DT_RELA=%#llx RELASZ=%#llx JMPREL=%#llx PLTRELSZ=%#llx SYMTAB=%#llx STRTAB=%#llx\n",
             mod0, (unsigned long long)dyn, (unsigned long long)DT_RELA, (unsigned long long)DT_RELASZ,
             (unsigned long long)DT_JMPREL, (unsigned long long)DT_PLTRELSZ,
             (unsigned long long)DT_SYMTAB, (unsigned long long)DT_STRTAB);
        klog("[重定位诊断] 映像前 16 字节: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
             img[0],img[1],img[2],img[3],img[4],img[5],img[6],img[7],img[8],img[9],img[10],img[11],img[12],img[13],img[14],img[15]);
        appf(out, cap, used, "  ✓ 重定位：RELATIVE %llu 条（共 %llu），非 RELATIVE %llu 条\n",
             (unsigned long long)n_rel, (unsigned long long)(DT_RELASZ / 24), (unsigned long long)n_other);
        appf(out, cap, used, "  ✓ SDK 槽打桩：%d 个 PLT 槽 → 记录型存根（调用即记名）\n", g_nplt);
    }
    if (dm_ohos_map_image_seal(&g_k_m)) { app(out, cap, used, "  ✗ seal 失败\n\n"); return; }
    appf(out, cap, used, "  ✓ 装载并封 R+X：base %p\n", (void *)g_k_m.base);

    for (int i = 0; i < 8; i++)
        if (pthread_key_create(&g_k_key[i], NULL)) { app(out, cap, used, "  ✗ 建键失败\n\n"); return; }
    uint64_t *page = (uint64_t *)(g_k_m.base + DM_IMG_MAIN_KEY_PAGE_OFF);
    memset(page, 0, DM_IMG_MAIN_KEY_PAGE_SIZE);
    page[DM_IMG_MAIN_KEY_K18_IDX]       = (uint64_t)g_k_key[0] * 8;
    page[DM_IMG_MAIN_KEY_KTLS_IDX]      = (uint64_t)g_k_key[1] * 8;
    page[DM_IMG_MAIN_KEY_KTLR_IDX]      = (uint64_t)g_k_key[2] * 8;
    page[DM_IMG_MAIN_KEY_KGATE_IDX]     = (uint64_t)g_k_key[3] * 8;
    page[DM_IMG_MAIN_KEY_KRES0_IDX]     = (uint64_t)g_k_key[4] * 8;
    page[DM_IMG_MAIN_KEY_KRES1_IDX]     = (uint64_t)g_k_key[5] * 8;
    page[DM_IMG_MAIN_KEY_KASPECT_IDX]   = (uint64_t)g_k_key[6] * 8;
    page[DM_IMG_MAIN_KEY_KASPECTUI_IDX] = (uint64_t)g_k_key[7] * 8;

    struct sigaction sa, o1, o2, o3, o4;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = gate_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1); sigaction(SIGBUS, &sa, &o2);
    sigaction(SIGILL, &sa, &o3);  sigaction(SIGTRAP, &sa, &o4);

    g_k_hits = 0; g_k_faulted = 0; g_k_rv = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, k_thread, NULL) == 0) pthread_join(th, NULL);
    else app(out, cap, used, "  ✗ pthread_create 失败\n");

    sigaction(SIGSEGV, &o1, NULL); sigaction(SIGBUS, &o2, NULL);
    sigaction(SIGILL, &o3, NULL);  sigaction(SIGTRAP, &o4, NULL);

    appf(out, cap, used, "  SDK 槽被真实调用 %d 次%s\n", g_plt_calls, g_plt_calls ? "（名见 dm_run_test.log）" : "");
    appf(out, cap, used, "  宿主堆被分配 %llu 次%s\n", (unsigned long long)g_alloc_calls,
         g_alloc_calls ? "（游戏分配器已通）" : "（未走到分配器）");
    appf(out, cap, used, "  %s 启动链跑完并返回（rv=%#llx）；门被命中 %d 次%s\n",
         g_k_faulted ? "✗" : "✓", (unsigned long long)g_k_rv, g_k_hits,
         g_k_faulted ? "（中途崩，崩点见 dm_run_test.log）" : "");
    if (g_k_faulted)
        appf(out, cap, used, "  崩点：pc %#llx sig %d（离线对符号可定位到函数）\n",
             (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
    appf(out, cap, used, "  （明细落盘 %s/dm_run_test.log）\n", files_dir ? files_dir : "?");
    if (g_k_log) { fclose(g_k_log); g_k_log = NULL; }
    dm_log_set_mirror(NULL);
    app(out, cap, used, "\n");
}

static void exp_h(const char *files_dir, char *out, size_t cap, size_t *used) {
    app(out, cap, used, "【实验 H】门回路：93 个真实门块 → dm_gate → gate_dispatch → svc 38 → 返回\n");
    if (files_dir && *files_dir) {
        char pth[512]; snprintf(pth, sizeof pth, "%s/dm_gate_test.log", files_dir);
        g_gate_log = fopen(pth, "w");
        dm_log_set_mirror(g_gate_log);   // 宿主侧全部 LOG 镜像进同一文件（mem_init/FATAL/Break# 都看得见）
    }
    setenv("DM_GUEST_AS_MB", "512", 1);   // 探针进程 VA 有限：门回路不碰堆/别名/栈，512MB 预留够用
    // stderr 重定向到文件：crash.c 的崩溃处理器把完整现场（pc + 29 寄存器）打到 stderr，
    // 而 OHOS 应用把 stderr 丢弃——重定向后现场落盘（filesDir/dm_crash.log），门回路崩一次即得全部线索。
    if (files_dir && *files_dir) {
        char cpth[512]; snprintf(cpth, sizeof cpth, "%s/dm_crash.log", files_dir);
        if (!freopen(cpth, "w", stderr)) appf(out, cap, used, "  ⚠ stderr 重定向失败\n");
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

    // ⑤ 二分 B（线程版，见 gb_thread）：门调度搬专用线程——验证主线程卡死看门狗假设
    {
        glog("二分 B（线程版）开始：将在线程里直调 gate_dispatch(ret=%#llx)\n",
             (unsigned long long)(m.base + DM_GATES_MAIN[0].ret));
        memset(&g_gb, 0, sizeof g_gb);
        g_gb.base = (uint64_t)(m.base + DM_GATES_MAIN[0].ret);
        memcpy(g_gb.key, key, sizeof g_gb.key);
        pthread_t th;
        if (pthread_create(&th, NULL, gb_thread, &g_gb) == 0) {
            pthread_join(th, NULL);
            glog("二分 B（线程版）：线程返回 rv=%#llx faulted=%d\n",
                 (unsigned long long)g_gb.rv, g_gb.faulted);
            appf(out, cap, used, "  %s 二分 B（线程版）：返回 %#llx%s\n",
                 (!g_gb.faulted && g_gb.rv == 0) ? "✓" : "✗",
                 (unsigned long long)g_gb.rv, g_gb.faulted ? "（线程内崩，兜底接住）" : "");
        } else {
            app(out, cap, used, "  ✗ 二分 B：pthread_create 失败\n");
        }
    }

    // ⑤ 二分 C：直调 svc_dispatch(38, regs2, orig)（跳过哈希查找）——分离「查找」与「处理器」
    {
        uint64_t regs3[64]; memset(regs3, 0, sizeof regs3);
        regs3[0] = 0x80000000ull;
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
            register uint64_t g_x0 __asm__("x0") = 0x80000000ull;
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
            // Break「仅通知」路径不改 x0（帧里的 x0 原样恢复）⇒ 期望值 = 我们喂的仅通知位
            if (rv == 0 || rv == 0x80000000ull) ok++;
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
    dm_log_set_mirror(NULL);
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
    // 2026-10-16 收窄：只跑「执行链探针」这一组（G 装载 / H 门回路 / I 真实执行 / J 门+执行 / K 启动链）。
    // D/E/E2/F 与 A/B/C 是早期探路实验（装载机制/图形链），已收官——函数留在代码里备查，但不再每轮跑，
    // 每轮省掉 mmap 试探与大量报告，迭代更快。
    logline(">>> 实验 G 开始"); exp_g(out, cap, &used); logline(">>> 实验 G 返回");
    logline(">>> 实验 H 开始"); exp_h(files_dir, out, cap, &used); logline(">>> 实验 H 返回");
    logline(">>> 实验 I 开始"); exp_i(out, cap, &used); logline(">>> 实验 I 返回");
    logline(">>> 实验 J 开始"); exp_j(out, cap, &used); logline(">>> 实验 J 返回");
    logline(">>> 实验 K 开始"); exp_k(files_dir, out, cap, &used); logline(">>> 实验 K 返回");

    app(out, cap, &used, "==================================================\n");
    app(out, cap, &used, "把这份报告发回来，就能定下装载方案。\n");
    logline(">>> 执行链探针全部跑完（G/H/I/J/K）");
    return 0;
}
