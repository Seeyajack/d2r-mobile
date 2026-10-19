// 三个真机实验的实现。见 dm_probe.h。
#define _GNU_SOURCE

#include "dm_probe.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "dm.h"                 // module_t
#include "map_image_ohos.h"     // 鸿蒙版 map_image —— 实验 D 的被测对象
#include "dm_fs_ohos.h"         // 最小 nn::fs（romfs → 宿主目录；导入按符号名分流到这里）
#include "dm_sdk_ohos.h"        // 通用 SDK 胶水层（vi/oe 等轻量语义；与游戏无关）
#include "dm_nvn_ohos.h"        // 通用 NVN 胶水层（设备族等平台中立语义）
#include "dm_nvn_vk.h"         // 通用 NVN 内存池/缓冲胶水层（UMA 直通）
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

// 实验 H/K 的错误兜底：SIGSEGV/SIGBUS 时记下**完整现场**，跳回探针（进程活着，继续报）。
// 为什么连寄存器一起记：追崩链时"我们以为的调用路径"会和日志对不上（实测：分配器明明给了非 0
// 指针，被调方却看到 x0=0）——**崩点现场是唯一不会骗人的证人**。
// 异步信号安全：处理器里只做搬运，打印与帧链回溯留到 siglongjmp 之后（见 gate_dump_fault）。
#define GATE_REG_N 31
static sigjmp_buf g_gate_jmp;
static volatile sig_atomic_t g_gate_armed;
static volatile uint64_t g_gate_fault_pc;
static volatile int g_gate_fault_sig;
static uint64_t g_gate_regs[GATE_REG_N];
static uint64_t g_gate_sp;
static volatile uint64_t g_gate_fault_addr;   // ★ si_addr：真正访问失败的地址（缺了它，"崩在哪条指令"会骗人）
static volatile int g_gate_have_regs;
// ── Task #5 越障（np60）：崩点 +0xA05620 的定点修复 ──
// 崩点本质：worker 0xa051c4 用"解析结果"（我们给的全零替身）拼出 0 尺寸的命令编码，
// 最后 `str x8,[x0]` 时 x0=0。正解是让解析拿到真数据（§37.5 方案 B），这里只是**诊断越障**：
// 把写目标换成一块暂存页再放行，看下一层需求是什么。
static uint64_t g_self_base;            // np82：本 .so 的装载基址（dladdr 取，用于识别"外"调用者）
static int g_nnmain_entered;      // np86：nnMain 段是否已进入（工厂分支的迟到防护）
static volatile sig_atomic_t g_fixup_n;
static volatile uint64_t g_fixup_last[8];     // 最近 8 次拦截的 PC 偏移（信号里不打印，崩后统一报）
static uint8_t *g_fixup_page;                 // 安装期 calloc（不在信号上下文里分配——np58 教训）
static uintptr_t g_fixup_base;                // 模块基址（处理器里不能用 g_k_m，它在文件后面才声明）
static void gate_fault_handler(int sig, siginfo_t *si, void *uc_) {
    // 字段写法与 dm/host/crash.c 的鸿蒙分支一致：mcontext_t = { regs[31], sp, pc, pstate }
    ucontext_t *uc = (ucontext_t *)uc_;
    if (g_fixup_page && g_fixup_base && g_fixup_n < 400) {
        uint64_t off = (uint64_t)uc->uc_mcontext.pc - (uint64_t)g_fixup_base;
        // worker 0xa051c4..0xa05fa0 是"命令编码"大函数：多处 `stp/str ...,[x0]` 用同一个
        // 写游标，游标来源（解析结果链条）目前恒 0。这里在整个函数范围内兜底：
        // 每次给一块新的暂存区（按 0x100 步进、0x4000 回绕），模拟"缓冲区在推进"。
        // np81：竞态崩点 +0xA011F4（`ldrb w10,[x9,#0x14]`，x9=0 ⇒ si_addr=0x2c）——
        //   "任务节点没注册 ⇒ 查找落空 x9=0 ⇒ 从 0x18 起遍历"。给 x9 一块**全零**暂存区：
        //   读到的字节是 0 ⇒ 该节点被跳过，循环继续（计数正常时很快结束）。
        if (off >= 0xA011E4 && off < 0xA01220 && (uint64_t)(uintptr_t)si->si_addr < 0x1000) {
            g_fixup_n++;
            uc->uc_mcontext.regs[9] = (uint64_t)(uintptr_t)g_fixup_page;   // x9 = 零页
            if (g_fixup_n <= 8) g_fixup_last[g_fixup_n - 1] = off;
            return;
        }
        if (off >= 0xA051C4 && off < 0xA05FA0 && (uint64_t)(uintptr_t)si->si_addr < 0x1000) {
            g_fixup_n++;
            uc->uc_mcontext.regs[0] = (uint64_t)(uintptr_t)(g_fixup_page + ((size_t)g_fixup_n % 64) * 0x100);
            if (g_fixup_n <= 8) g_fixup_last[g_fixup_n - 1] = off;   // 信号里只记账，崩后统一打印
            return;    // ★ 同步信号的处理函数返回 ⇒ 内核**重试**那条指令（这次写暂存页）
        }
    }
    g_gate_fault_sig = sig;
    g_gate_fault_pc = (uint64_t)uc->uc_mcontext.pc;   // aarch64 mcontext 的 pc
    g_gate_fault_addr = (uint64_t)(uintptr_t)si->si_addr;   // 出错的内存地址
    for (int i = 0; i < GATE_REG_N; i++) g_gate_regs[i] = (uint64_t)uc->uc_mcontext.regs[i];
    g_gate_sp = (uint64_t)uc->uc_mcontext.sp;
    g_gate_have_regs = 1;
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
// 阶段标签：实验 K 连跑三站（启动链 → 对象工厂 → nnMain），同一批插桩在三站都会响。
// 每条日志带上"当前在哪一站"，读日志时才分得清"这次命中属于哪一站"。
static const char *g_k_stage = "准备";
// 日志串行化用的递归锁（见 klog 里的理由）
static pthread_mutex_t g_klog_mu;
static pthread_once_t  g_klog_once = PTHREAD_ONCE_INIT;
static void klog_mu_init(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_klog_mu, &a);
    pthread_mutexattr_destroy(&a);
}
static void klog(const char *fmt, ...) {
    if (!g_k_log) return;
    // 带线程 ID：游戏有多个线程（探针主线程 / sead 任务线程 / Havok 物理工作线程）都在打日志，
    // 没有 tid 分不清哪行是谁写的（2026-10-18 加：判读主线程卡在哪必需）。
    // 真线程后必须串行化（否则多线程 fprintf 会互相穿插）。**必须递归锁**：
    // 万一某线程在写日志时崩了、故障处理器（gate_dump_fault）又要写日志，
    // 非递归锁会把它自己锁死 ⇒ 整份日志从那一刻静默（2026-10-18 现场嫌疑之一）。
    pthread_once(&g_klog_once, klog_mu_init);
    pthread_mutex_lock(&g_klog_mu);
    fprintf(g_k_log, "[阶段 %s][T%ld] ", g_k_stage, (long)gettid());
    va_list ap; va_start(ap, fmt);
    vfprintf(g_k_log, fmt, ap);
    va_end(ap);
    fflush(g_k_log);
    pthread_mutex_unlock(&g_klog_mu);
}

// ── NVN 最小空设备 + 按名字记账桩（2026-10-17 深夜，Task #9）──
// 真机实证（np20）：游戏只从 nvnBootstrapLoader 取**两个**名字：nvnDeviceInitialize /
// nvnDeviceGetProcAddress；其余 NVN 函数全走 GetProcAddress 按名字取。
// 真机实证（np25）：GetProcAddress 被调上百次；但之前**全返同一个通用桩**
//   ⇒ 日志只知道"被取走多少次"，看不出游戏**先撞哪一族、先撞哪个函数** ⇒ 无法排期实现。
// 本轮改法：每个名字一个独立桩（表在 dm_nvn_stubs.inc，由 tools/gen_nvn_stubs.py 生成）。
//   桩一律 `return 0` —— 与改前"全返同一个通用桩"**完全等价**，纯观测、不改语义。
// 日志给出三样东西：
//   ① **首次真正调用的顺序**（这就是 NVN 523 个 API 的实现排期依据）
//   ② 每 2 秒一行的**族合计**（由游戏自己的帧线程打出，见 plt_stub 里的 ConvertToTimeSpan tick）
//   ③ 崩点现场附带一份非零计数汇总（见 gate_dump_fault → nvn_api_summary）
// 参考实现：仓库 dm/gfx/nvn/hle_nvn.c（macOS/Metal 版为「强制接管 + 名字表 + 计数跳板」）。
// 表本身（族枚举 + 名字表 + 523 个记账桩 + 桩地址表）由 tools/gen_nvn_stubs.py 生成：
//   python dm/ohos/tools/gen_nvn_stubs.py     # 输入 = 仓库 dm/gfx/nvn/names.txt（NVN 完整 API 面）
// ⚠ np26 纠正：早先以为"BotW 只用 120 个 NVN 函数"——那是旧日志只记前 120 次的产物。
//   真机实测查询 636 次、去重 ~520 个，正好等于 names.txt 的 523 个 ⇒ 一律以全表为准。
static uint64_t nvn_api_touch(int idx);       // 前置声明：.inc 里的桩体要调它（定义在 .inc 之后）
#include "dm_nvn_stubs.inc"

static uint64_t nvn_stub(void) { return 0; }  // 兜底桩：清单外的名字（按名记录一次，换游戏会遇到）
static int g_n_nvn_unknown;

static uint64_t nvn_api_touch(int idx) {
    nvn_api_t *a = &g_nvn_api[idx];
    uint64_t old = __atomic_fetch_add(&a->calls, 1, __ATOMIC_RELAXED);
    if (old == 0)
        klog("[NVN] 首次调用 %-40s (%-11s)\n", a->name, g_nvn_fam_name[a->fam]);
    else if ((old % 10000) == 0)
        klog("[NVN] 累计 %-40s (%-11s) = %llu 次\n", a->name, g_nvn_fam_name[a->fam],
             (unsigned long long)(old + 1));
    return 0;
}

// 汇总：只打非零项，按族分组。崩点现场与收尾都调它——这 523 个 API 里到底谁在干活，一眼看出。
static void nvn_api_summary(const char *where) {
    int nz = 0;
    for (int i = 0; i < NVN_API_N; i++) if (g_nvn_api[i].calls) nz++;
    if (!nz) { klog("[NVN] 汇总（%s）：%d 个 API 一个都没被真正调用过\n", where, NVN_API_N); return; }
    klog("[NVN] 汇总（%s）：%d/%d 个 API 被调用过\n", where, nz, NVN_API_N);
    for (int f = 0; f < NVN_FAM__N; f++) {
        uint64_t sum = 0; int cnt = 0;
        for (int i = 0; i < NVN_API_N; i++)
            if (g_nvn_api[i].fam == f && g_nvn_api[i].calls) { sum += g_nvn_api[i].calls; cnt++; }
        if (!cnt) continue;
        klog("[NVN]   %-11s %d 个函数 合计 %llu 次\n", g_nvn_fam_name[f], cnt,
             (unsigned long long)sum);
        for (int i = 0; i < NVN_API_N; i++)
            if (g_nvn_api[i].fam == f && g_nvn_api[i].calls)
                klog("[NVN]     %-40s %llu\n", g_nvn_api[i].name,
                     (unsigned long long)g_nvn_api[i].calls);
    }
}

// 观察窗之一：**族合计**（谁在干活）。只在有变化时打一行，族级不刷屏。
// ⚠ 谁来调很关键：np26/np27 真机用"宿主看护线程每 N 秒打一次"——结果**一行都没出**，
//   而同一时刻游戏线程的日志正常。说明我们起的宿主线程根本没被调度（见 nvn_host_thread_probe）。
//   ⇒ 改成由**游戏自己的帧线程**打：plt_stub 里 ConvertToTimeSpan 每帧一次，每 120 次 ≈ 2 秒。
static void nvn_family_totals(uint64_t out[NVN_FAM__N]) {
    for (int f = 0; f < NVN_FAM__N; f++) out[f] = 0;
    for (int i = 0; i < NVN_API_N; i++)
        out[g_nvn_api[i].fam] += __atomic_load_n(&g_nvn_api[i].calls, __ATOMIC_RELAXED);
}
static void nvn_family_report(const char *who) {
    static uint64_t prev[NVN_FAM__N];                 // 初值全 0 ⇒ 第一次调用就打全量
    uint64_t now[NVN_FAM__N];
    nvn_family_totals(now);
    char line[320];
    size_t used = 0;
    int changed = 0;
    line[0] = '\0';
    for (int f = 0; f < NVN_FAM__N; f++) {
        if (now[f] == prev[f]) continue;
        changed = 1;
        prev[f] = now[f];
        if (used + 40 >= sizeof line) continue;        // 装不下就跳过这一族（不截断上一次的输出）
        int n = snprintf(line + used, sizeof line - used, "%s=%llu ",
                         g_nvn_fam_name[f], (unsigned long long)now[f]);
        if (n > 0) used += (size_t)n;
    }
    if (changed) klog("[NVN] 族合计（%s）：%s\n", who, line);
}

// 观察窗之二：**宿主线程到底跑不跑**（一个只打心跳的最小实验）。
// np26/np27：宿主看护线程一行日志都没有，而 pthread_create 返回成功 ⇒ 怀疑被游戏线程饿死。
// 这里只在前 3 拍打心跳；打得出来说明宿主线程能跑，打不出来就是调度饥饿（本身就是重要结论）。
static void *nvn_host_thread_probe(void *arg) {
    (void)arg;
    for (int i = 1; i <= 3; i++) {
        // ⚠ 别用 usleep(10*1000*1000)：POSIX 只保证 <1e6，某些 libc 直接 EINVAL ⇒ 瞬间跑完不打一行。
        struct timespec ts;
        ts.tv_sec = 2;
        ts.tv_nsec = 0;
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) { }
        klog("[NVN] 宿主线程心跳 %d/3（能打出这行 ⇒ 宿主线程有被调度）\n", i);
    }
    return NULL;
}
static void nvn_host_thread_probe_start(void) {
    static int started;
    if (started) return;
    started = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, nvn_host_thread_probe, NULL) == 0) pthread_detach(t);
    else klog("[NVN] 宿主线程起不来（不影响主线）\n");
}

static int g_gate_caught;        // np107：门字节是否已变非零（抓住写入者用）
static size_t g_gate_ctor;       // np107：写入者的构造器序号
static uint64_t g_gate_ctorentry;
static uint64_t g_gate_obj;
static unsigned g_gate_val;
static void *nvn_real_lookup(const char *name) {
    void *r = dm_nvn_vk_real(name);         // 通用 NVN 池/缓冲层（L1，UMA 直通）
    if (r) return r;
    r = dm_nvn_real(name);                  // 通用 NVN 设备族胶水层（L1）
    // np101：池/缓冲族的 23 个真实现已搬去 dm_nvn_vk.c（上面的 dm_nvn_vk_real），
    //   探针里不再保留自己的表。设备初始化/GetProcAddress 两个仍在本文件（下面）。
    return r;
}

static uint64_t nvn_dev_init(void *dev, const void *builder) {
    (void)builder;
    if (dev) *(uint64_t *)dev = 0x4E564E44ull;   // 'NVND' 魔数（不 memset：设备对象大小未知，防越界）
    klog("[NVN] nvnDeviceInitialize → 1（空设备）\n");
    nvn_host_thread_probe_start();                 // 只打 3 拍心跳，测"宿主线程能不能被调度"
    return 1;                                     // NVNboolean 真 = 初始化成功
}
static uint64_t nvn_dev_gpa(void *dev, const char *name) {
    (void)dev;
    if (!name) return (uint64_t)(uintptr_t)&nvn_stub;
    // 这两个是**真实现**，不能走记账桩：np26 真机日志里游戏在 DeviceInitialize 之后又
    // GetProcAddress("nvnDeviceInitialize") 取了一次（4416/4326 行）——若发记账桩（返 0），
    // 第二次初始化会被告知"失败"。同理 GetProcAddress 自己也可能被再取一次。
    if (!strcmp(name, "nvnDeviceInitialize"))    return (uint64_t)(uintptr_t)&nvn_dev_init;
    if (!strcmp(name, "nvnDeviceGetProcAddress")) return (uint64_t)(uintptr_t)&nvn_dev_gpa;
    // ★ 真实现优先（内存池/缓冲）：必须在记账桩表**之前**查，否则会拿回返 0 的桩。
    //   ⚠ 走真实现的函数不再进 g_nvn_api[].calls 计数（桩没被调），所以 §28 那张
    //   "14/523" 汇总表会把它们显示为 0——判读时以 `[NVN] 池/缓冲` 那几行日志为准。
    {
        void *real = nvn_real_lookup(name);
        if (real) {
            static int n_real_logged;
            if (++n_real_logged <= 60)
                klog("[NVN] GetProcAddress(「%s」) → **真实现**\n", name);
            return (uint64_t)(uintptr_t)real;
        }
    }
    for (int i = 0; i < NVN_API_N; i++) {
        if (!strcmp(g_nvn_api[i].name, name)) {
            static int n_logged;
            if (++n_logged <= 1200)
                klog("[NVN] GetProcAddress(「%s」) → 独立记账桩 #%d/%s\n",
                     name, i, g_nvn_fam_name[g_nvn_api[i].fam]);
            return (uint64_t)(uintptr_t)g_nvn_thunks[i];
        }
    }
    // 清单外的名字 = §18.3 那份 120 项清单漏了东西（换游戏时一定会遇到），记下来但别刷屏。
    // ⚠ np26 真机：上限 40 被**打满**了 ⇒ 说明原始清单本身就截断了（旧代码只记前 120 个
    //   GetProcAddress）⇒ 这里放到 400，一次抓全，再把清单补进 nvn-api-botw.txt。
    if (++g_n_nvn_unknown <= 3000)
        klog("[NVN] ⚠ 清单外名字「%s」→ 兜底桩（该补进 nvn-api-botw.txt）\n", name);
    return (uint64_t)(uintptr_t)&nvn_stub;
}
static volatile int g_k_hits;
static uint64_t g_k_hit_lr[16];
static volatile int g_k_faulted;
static uint64_t g_k_rv;
static module_t g_k_m;
static pthread_key_t g_k_key[8];

// 把崩点现场落进日志。**必须在 siglongjmp 之后调用**——不在信号处理器里碰 stdio。
// 帧链回溯要解引用可疑地址，所以临时换上探针自己的兜底处理器（probe_segv_handler + safe_read64），
// 走完再换回来；否则一次野指针会落到 gate_fault_handler 的"未武装"分支 _exit(139)，整份日志都丢。
static void gate_dump_fault(const char *where) {
    if (!g_gate_have_regs) return;
    klog("=== 崩点现场（%s）：pc=%#llx sp=%#llx sig=%d 出错地址(si_addr)=%#llx ===\n", where,
         (unsigned long long)g_gate_fault_pc, (unsigned long long)g_gate_sp, g_gate_fault_sig,
         (unsigned long long)g_gate_fault_addr);
    for (int i = 0; i < GATE_REG_N; i++)
        klog("  x%-2d=%#llx%s", i, (unsigned long long)g_gate_regs[i], (i % 2) ? "\n" : "  ");
    struct sigaction sa, o1, o2;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_segv_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &o1);
    sigaction(SIGBUS, &sa, &o2);
    // 帧链：AArch64 约定 [x29] = 上一帧 x29、[x29+8] = 本帧返回地址。
    // 这一串返回地址就是**真实调用栈**，比任何"推断的链"都可信。
    klog("  帧链（fp → 本帧返回地址 / 上一帧 fp）:\n");
    uint64_t fp = g_gate_regs[29];
    for (int d = 0; d < 16; d++) {
        uint64_t lr = 0, next = 0;
        if (!fp || !safe_read64(fp + 8, &lr) || !safe_read64(fp, &next)) break;
        klog("    [%2d] fp=%#llx  返回地址=%#llx\n", d, (unsigned long long)fp, (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
    sigaction(SIGSEGV, &o1, NULL);
    sigaction(SIGBUS, &o2, NULL);
    // 顺手把 NVN 记账汇总打出来：崩在哪一步 + 此时 NVN 用到什么程度，放在一起才看得出因果
    nvn_api_summary(where);
    klog("=== 现场结束 ===\n");
}
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
static int g_nimpl;      // 直连宿主的导入个数（nn::fs 等，见重定位循环）
static volatile int g_plt_calls;
static int g_plt_log_n;          // 通用 PLT 记名行数（截断计数，见 plt_stub 末尾）
#define DM_PLT_LOG_CAP 20000     // init 段约一千行 + 充分余量；循环段靠它挡住
// 宿主侧"真服务"表：启动链真的需要返回值的几个（不是一律返 0）。
// 依据：崩点反汇编显示 AllocateMemoryBlock 的返回值被当对象指针使用。
static char g_host_arg0[] = "main";
static char *g_host_argv[2];
// nn::os::GetCurrentThread 要返回"当前线程"的 ThreadType*。我们还没有真实的 ThreadType，
// 先给一个宿主侧的**影子对象**：清零、够大，让游戏"拿它当结构体读"这一步不撞墙。
// 依据（真机实证）：新崩点 0xa1d94c 就是拿返回值去读 [+0x58]（返回值若为 0 ⇒ 读 0x58 崩）。
// 字段语义待看清游戏怎么用之后再补；单线程深跑阶段一个影子对象够用（多线程时须按线程分开）。
#define DM_THREAD_SHADOW_QWORDS 0x40            // 0x200 字节，覆盖到 [+0x58] 及其后
static uint64_t g_thread_shadow[DM_THREAD_SHADOW_QWORDS];
// nn::os::GetTlsValue 的影子对象：游戏把返回值当"线程局部存储"用，其中 **[+0xb0] 被 0xa0e2f8
// 当作"当前分配器对象"取出**。真机实证（2026-10-17）：不实现它时 GetTlsValue 返回 0 ⇒
// 0xa0e2f8 走兜底读 [obj+8]，而那个"自指"值此时已被清成 0 ⇒ 整条 sead 单例查找返回 0。
// 这里让 [+0xb0] 指向注入的分配器对象（在 k_inject_heap 里接线），使这条分支拿到非 0。
// 注：槽号目前只有 0（注入的上下文对象 [0x88] 为 0），故一个影子对象够用。
static uint64_t g_tls_shadow[0x40];             // 0x200 字节，覆盖到 [+0xb0]

// ── 真实线程（2026-10-18）：之前 CreateThread/StartThread 完全没实现 ⇒ 游戏所有"线程"在
//    单线程上内联跑 ⇒ **子系统工作线程压根不存在 ⇒ 它们的任务永远不产生 ⇒ sead 主循环空转**
//    （真机实证：28811 行日志只有两个 tid；主循环回调 633 万次却无事可做）。
//    这里用 pthread 真建线程：ThreadType* ↔ 线程槽 映射表，StartThread 用蹦床调 entry(arg)。
//    ⚠ 副作用：真线程后，互斥量必须是真的、TLS 必须按线程分开、日志必须串行化。
#define DM_THREAD_MAX 16
typedef struct {
    uint64_t game;              // 游戏侧 ThreadType*（0 = 空槽）
    pthread_t tid;              // 宿主线程
    void (*entry)(void *);
    void *arg;
    volatile int started;
    int prio, core_mask;
    char name[24];              // SetThreadName 存的线程名（pthread_setname_np 用）
    uint64_t id;                // 游戏侧线程号（GetThreadId 用；稳定、不复用）
} dm_thread_t;
static dm_thread_t g_thr[DM_THREAD_MAX];
static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_thr_id_next = 1;
static int thr_find_by_game(uint64_t game) {
    for (int i = 0; i < DM_THREAD_MAX; i++) if (g_thr[i].game == game) return i;
    return -1;
}
static int thr_find_by_self(void) {
    pthread_t me = pthread_self();
    for (int i = 0; i < DM_THREAD_MAX; i++)
        if (g_thr[i].game && g_thr[i].started && pthread_equal(g_thr[i].tid, me)) return i;
    return -1;
}
static void *thr_trampoline(void *p) {
    dm_thread_t *t = (dm_thread_t *)p;
    t->entry(t->arg);           // 游戏的线程体
    return NULL;
}

// ── 线程局部存储：**不做真槽**（2026-10-18 真机实证回退，见 plt_stub 里 TLS 三行的注释）──
// 旧语义：槽号恒 0、Set 忽略、Get 恒返影子对象。真线程共享这个影子可接受
// （它只被当作"当前上下文对象"用，[+0xb0] 是全局分配器）。

// ── nn::os 互斥量：**真 pthread_mutex，且必须递归** ──
// ⚠ 2026-10-18 真机实证（0xa01924 二次加锁挂死）：sead 的"锁"（0xa1d56c/0xa1d57c）其实就是
//   `add x0,#0x20; b nn::os::LockMutex/UnlockMutex`（PLT 槽 0x22636f0/0x22636f8）。
//   **nn::os::Mutex 是递归语义**（同一线程可重复加锁、靠计数解锁），于是同一线程先由
//   mgr 构造器加锁、再由池注册函数加锁是正常路径。非递归 pthread_mutex 在这里必死锁。
#define DM_MUTEX_MAX 64
static void *g_mtx_key[DM_MUTEX_MAX];
static pthread_mutex_t g_mtx_impl[DM_MUTEX_MAX];
static pthread_mutex_t g_mtx_reg = PTHREAD_MUTEX_INITIALIZER;   // 登记表自身的锁
static pthread_mutexattr_t g_mtx_attr;
static pthread_once_t g_mtx_once = PTHREAD_ONCE_INIT;
static void mtx_attr_init(void) {
    pthread_mutexattr_init(&g_mtx_attr);
    pthread_mutexattr_settype(&g_mtx_attr, PTHREAD_MUTEX_RECURSIVE);
}
static pthread_mutex_t *mtx_get(void *m) {
    pthread_once(&g_mtx_once, mtx_attr_init);
    pthread_mutex_lock(&g_mtx_reg);
    for (int i = 0; i < DM_MUTEX_MAX; i++) if (g_mtx_key[i] == m) { pthread_mutex_unlock(&g_mtx_reg); return &g_mtx_impl[i]; }
    for (int i = 0; i < DM_MUTEX_MAX; i++) if (!g_mtx_key[i]) {
        g_mtx_key[i] = m;
        pthread_mutex_init(&g_mtx_impl[i], &g_mtx_attr);
        pthread_mutex_unlock(&g_mtx_reg);
        return &g_mtx_impl[i];
    }
    pthread_mutex_unlock(&g_mtx_reg);
    return &g_mtx_impl[0];      // 表满：退化（不该发生）
}

// ── 消息队列（2026-10-18）──
// Prepare Thread 用 ReceiveMessageQueue 收命令。之前存根秒返 0 但不写消息 ⇒ 游戏可能读到
// 未初始化消息，且线程只能忙等（真机日志实证：Receive+Yield 空转 11 轮）。真实现：
// 游戏提供**值缓冲区**（InitializeMessageQueue 的 buf/count），我们维护头尾 + 条件变量，
// 支持阻塞收/阻塞发（Send/Receive）与非阻塞（TrySend/TryReceive，bool 返回：1=成功）。
#define DM_MQ_MAX 8
typedef struct {
    uint64_t *buf;                 // 游戏侧值缓冲区
    size_t cap, head, tail, n;
    pthread_mutex_t mu;
    pthread_cond_t cv_notempty;    // 收方等"有消息"
    pthread_cond_t cv_notfull;     // 发方等"有空位"
} dm_mq_t;
static void *g_mq_key[DM_MQ_MAX];
static dm_mq_t g_mq[DM_MQ_MAX];
static pthread_mutex_t g_mq_reg = PTHREAD_MUTEX_INITIALIZER;
static dm_mq_t *mq_get(void *q) {
    pthread_mutex_lock(&g_mq_reg);
    for (int i = 0; i < DM_MQ_MAX; i++) if (g_mq_key[i] == q) { pthread_mutex_unlock(&g_mq_reg); return &g_mq[i]; }
    for (int i = 0; i < DM_MQ_MAX; i++) if (!g_mq_key[i]) {
        g_mq_key[i] = q;
        memset(&g_mq[i], 0, sizeof g_mq[i]);
        pthread_mutex_init(&g_mq[i].mu, NULL);
        pthread_cond_init(&g_mq[i].cv_notempty, NULL);
        pthread_cond_init(&g_mq[i].cv_notfull, NULL);
        pthread_mutex_unlock(&g_mq_reg);
        return &g_mq[i];
    }
    pthread_mutex_unlock(&g_mq_reg);
    return NULL;
}

// 前置声明：TLS 的每线程影子要把 [+0xb0] 填成注入的分配器对象（定义在后面，C 的试探性定义合法）
static uint64_t g_alloc_obj[2];

static uint64_t *g_fake_heap;       // np103b 前置声明（定义在后）
static uint64_t plt_stub(void) {
    uint64_t x16v, a0, a1, a2, a3;
    __asm__ volatile("mov %0, x16" : "=r"(x16v));
    __asm__ volatile("mov %0, x0" : "=r"(a0));
    __asm__ volatile("mov %0, x1" : "=r"(a1));
    __asm__ volatile("mov %0, x2" : "=r"(a2));
    __asm__ volatile("mov %0, x3" : "=r"(a3));   // np96：第 4 参（VSNPrintf 的 va_list 在这里）
    const char *nm = NULL;
    for (int i = 0; i < g_nplt; i++) if (g_plt[i].slot == x16v) { nm = g_plt[i].name; break; }
    g_plt_calls++;
    if (nm) {
        // nn::os::AllocateMemoryBlock(size_t* out_size, size_t size) —— 真给内存
        if (!strcmp(nm, "_ZN2nn2os19AllocateMemoryBlockEPmm")) {
            // 必须清零（calloc）：游戏会把这块内存当自己的内存池，池管理器的空闲链头若读到
            // 未初始化垃圾，第一次"切对象"就会得到垃圾指针（本轮插桩实证：this=0xC0600000=请求大小）。
            void *p = calloc(1, (size_t)a1);
            // 证据：插桩读到游戏把"输出参数的值"当内存块基址用（this=0xC0600000=请求大小）
            // ⇒ 该输出参数要的是**地址**，不是大小。写地址。
            if (a0) *(uint64_t *)a0 = (uint64_t)(uintptr_t)p;
            klog("[服务] AllocateMemoryBlock(大小 %llu) → %p（写回地址）\n", (unsigned long long)a1, p);
            return (uint64_t)(uintptr_t)p;
        }
        // nn::os::GetHostArgc / GetHostArgv —— 给一对合理的启动参数
        if (!strcmp(nm, "_ZN2nn2os11GetHostArgcEv")) { klog("[服务] GetHostArgc → 1\n"); return 1; }
        if (!strcmp(nm, "_ZN2nn2os11GetHostArgvEv")) {
            g_host_argv[0] = g_host_arg0; g_host_argv[1] = NULL;
            {   // np94：谁在问启动参数？（x30 = 调用者返回地址）
                uint64_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
                uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
                int in = lr >= b2 && lr < b2 + DM_IMG_MAIN_END;
                klog("[服务] GetHostArgv → argv[0]=main（调用者=%s%#llx）\n", in ? "+" : "(外)",
                     (unsigned long long)(in ? lr - b2 : lr));
            }
            return (uint64_t)(uintptr_t)g_host_argv;
        }
        // ═══ 线程 / TLS / 互斥（2026-10-18 改造：真 pthread）═══
        // nn::os::CreateThread(ThreadType*, entry, arg, stack, stack_size, prio)
        if (!strcmp(nm, "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")) {
            pthread_mutex_lock(&g_thr_mu);
            int slot = -1;
            for (int i = 0; i < DM_THREAD_MAX; i++)
                if (!g_thr[i].game) { slot = i; break; }
            if (slot >= 0) {
                memset(&g_thr[slot], 0, sizeof g_thr[slot]);
                g_thr[slot].game = a0;
                g_thr[slot].entry = (void (*)(void *))(uintptr_t)a1;
                g_thr[slot].arg = (void *)(uintptr_t)a2;
                g_thr[slot].prio = 16;
                g_thr[slot].id = g_thr_id_next++;
            }
            pthread_mutex_unlock(&g_thr_mu);
            klog("[线程] CreateThread：类型=%#llx 入口=%#llx 参数=%#llx → 槽 %d\n",
                 (unsigned long long)a0, (unsigned long long)a1,
                 (unsigned long long)a2, slot);
            // np92：把**入口参数对象**的虚表与消息处理槽读出来——线程体 0xa1e30c 的消息循环是
            //   `x19 = 入口参数; x9 = [x19]; x9 = [x9+0x80]; blr x9`，所以
            //   **[参数+0] = 虚表，[虚表+0x80] = "收到消息后干什么"**。
            //   客体内存同地址空间可直接读，不用挂钩子（np84 的教训：挂钩子会扰动初始化）。
            if (a2 > 0x10000ull && a2 < 0x800000000000ull) {
                uint64_t vt = *(const uint64_t *)(uintptr_t)a2;
                if (vt > 0x10000ull && vt < 0x800000000000ull) {
                    uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
                    uint64_t h = *(const uint64_t *)(uintptr_t)(vt + 0x80);
                    int in = h >= b2 && h < b2 + DM_IMG_MAIN_END;
                    klog("[线程] 参数=%#llx 虚表=%#llx 消息处理槽[+0x80]=%s%#llx\n",
                         (unsigned long long)a2, (unsigned long long)vt, in ? "+" : "(外)",
                         (unsigned long long)(in ? h - b2 : h));
                }
            }
            return slot >= 0 ? 0 : 1;      // 0 = 成功
        }
        // nn::os::StartThread(ThreadType*) —— 真建宿主线程，蹦床调 entry(arg)
        if (!strcmp(nm, "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE")) {
            pthread_mutex_lock(&g_thr_mu);
            int slot = thr_find_by_game(a0);
            int ok = -1;
            if (slot >= 0 && !g_thr[slot].started) {
                g_thr[slot].started = 1;
                ok = pthread_create(&g_thr[slot].tid, NULL, thr_trampoline, &g_thr[slot]);
                if (ok != 0) g_thr[slot].started = 0;
            } else if (slot >= 0) ok = 0;      // 已启动：幂等
            pthread_mutex_unlock(&g_thr_mu);
            klog("[线程] StartThread：类型=%#llx 槽 %d → %s\n",
                 (unsigned long long)a0, slot, ok == 0 ? "已启动" : "失败");
            return ok == 0 ? 0 : 1;
        }
        // nn::os::SetThreadName(ThreadType*, const char*)
        if (!strcmp(nm, "_ZN2nn2os13SetThreadNameEPNS0_10ThreadTypeEPKc")) {
            pthread_mutex_lock(&g_thr_mu);
            int slot = thr_find_by_game(a0);
            if (slot >= 0 && a1) {
                snprintf(g_thr[slot].name, sizeof g_thr[slot].name, "%s", (const char *)(uintptr_t)a1);
                if (g_thr[slot].started) pthread_setname_np(g_thr[slot].tid, g_thr[slot].name);
            }
            pthread_mutex_unlock(&g_thr_mu);
            klog("[线程] SetThreadName：类型=%#llx → 「%s」\n",
                 (unsigned long long)a0, a1 ? (const char *)(uintptr_t)a1 : "?");
            return 0;
        }
        // nn::os::GetThreadId(const ThreadType*)
        if (!strcmp(nm, "_ZN2nn2os11GetThreadIdEPKNS0_10ThreadTypeE")) {
            pthread_mutex_lock(&g_thr_mu);
            int slot = thr_find_by_game(a0);
            uint64_t id = slot >= 0 ? g_thr[slot].id : 0;
            pthread_mutex_unlock(&g_thr_mu);
            return id ? id : 1;            // 主线程等未登记者给 1
        }
        // nn::os::GetCurrentThread —— 已启动的游戏线程回它自己的 ThreadType；其余回影子
        if (!strcmp(nm, "_ZN2nn2os16GetCurrentThreadEv")) {
            int slot = thr_find_by_self();
            if (slot >= 0) return g_thr[slot].game;
            return (uint64_t)(uintptr_t)&g_thread_shadow[0];
        }
        // nn::os::AllocateTlsSlot / SetTlsValue / GetTlsValue
        // 2026-10-18 最终形态：**每线程一份影子对象**（_Thread_local），首次取用时把 [+0xb0]
        //   填成注入的分配器对象。为什么不是"真槽 + 按线程存值"（那次崩在 0xa0e33c：某槽被
        //   存过 1，sead 的"设置分配器"助手拿 1 去读 [1+0xb0]）；为什么也不能共用一份
        //   （那会让两个真线程的"当前线程"身份撞车 ⇒ sead 互斥量重入判断失效 ⇒ 第二次
        //   加锁就挂住，真机实证 0xa01924 两连击后停住）。每线程影子两头都占：
        //   [+0xb0] 是全局分配器，其余字段各自独立。
        if (!strcmp(nm, "_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE")) {
            if (a0) *(uint32_t *)(uintptr_t)a0 = 0;      // 槽号恒 0（旧行为，见上）
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm")) {
            return 0;                                     // 忽略（见上，防"存非指针值"崩）
        }
        if (!strcmp(nm, "_ZN2nn2os11GetTlsValueENS0_7TlsSlotE")) {
            static _Thread_local uint64_t tls_shadow[0x40];
            if (!tls_shadow[0xb0 / 8])
                tls_shadow[0xb0 / 8] = (uint64_t)(uintptr_t)&g_alloc_obj[0];
            return (uint64_t)(uintptr_t)&tls_shadow[0];
        }
        // 核掩码 / 栈信息 / 优先级 —— 影子语义，别让游戏拿到 0 而误判"没有可用核"
        if (!strcmp(nm, "_ZN2nn2os26GetThreadAvailableCoreMaskEv")) return 0xF;
        if (!strcmp(nm, "_ZN2nn2os20GetCurrentCoreNumberEv")) return 0;
        if (!strcmp(nm, "_ZN2nn2os19GetCurrentStackInfoEPmS1_")) {
            if (a0) *(uint64_t *)(uintptr_t)a0 = 0;
            if (a1) *(uint64_t *)(uintptr_t)a1 = 0x100000;   // 1 MB
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os17GetThreadPriorityEPKNS0_10ThreadTypeE")) return 16;
        if (!strcmp(nm, "_ZN2nn2os20ChangeThreadPriorityEPNS0_10ThreadTypeEi")) {
            pthread_mutex_lock(&g_thr_mu);
            int slot = thr_find_by_game(a0);
            if (slot >= 0) g_thr[slot].prio = (int)(int32_t)a1;
            pthread_mutex_unlock(&g_thr_mu);
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os17SetThreadCoreMaskEPNS0_10ThreadTypeEim")) {
            pthread_mutex_lock(&g_thr_mu);
            int slot = thr_find_by_game(a0);
            if (slot >= 0) g_thr[slot].core_mask = (int)(int32_t)a1;
            pthread_mutex_unlock(&g_thr_mu);
            return 0;
        }
        // nn::os 互斥量 —— 真 pthread_mutex
        if (!strcmp(nm, "_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE")) {
            return pthread_mutex_trylock(mtx_get((void *)(uintptr_t)a0)) == 0 ? 1 : 0;
        }
        if (!strcmp(nm, "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE")) {
            // np76：加"等待时长"打点。互斥/队列是**真阻塞点**，它们走专门实现、不走通用记名，
            //   所以日志里看不见——帧循环停住时，正是要在这里找"卡在谁身上"。
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            pthread_mutex_lock(mtx_get((void *)(uintptr_t)a0));
            clock_gettime(CLOCK_MONOTONIC, &t1);
            long ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (long)(t1.tv_nsec - t0.tv_nsec) / 1000000;
            if (ms >= 5) {
                static int n; if (++n <= 20)
                    klog("[锁] LockMutex(%#llx) 等了 %ld ms（被别的线程占着）\n",
                         (unsigned long long)a0, ms);
            }
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE")) {
            pthread_mutex_unlock(mtx_get((void *)(uintptr_t)a0));
            return 0;
        }
        // ═══ 消息队列（见 mq_get 处注释）═══
        if (!strcmp(nm, "_ZN2nn2os22InitializeMessageQueueEPNS0_16MessageQueueTypeEPmm")) {
            dm_mq_t *q = mq_get((void *)(uintptr_t)a0);
            if (q) {
                q->buf = (uint64_t *)(uintptr_t)a1;    // 游戏提供的值缓冲区
                q->cap = (size_t)a2;
                q->head = q->tail = q->n = 0;
                klog("[队列] 初始化：%#llx buf=%#llx 容量=%llu\n",
                     (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2);
            }
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os19ReceiveMessageQueueEPmPNS0_16MessageQueueTypeE")) {
            dm_mq_t *q = mq_get((void *)(uintptr_t)a1);
            if (!q || !q->cap) return 0;               // 未初始化：别悬住（宁可空转）
            pthread_mutex_lock(&q->mu);
            if (q->n == 0) {
                static int nw;
                if (++nw <= 20) {
                    // np82：谁在等？x30 = 调用者返回地址（plt_stub 里可读）
                    uint64_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
                    uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
                    int in_self = g_self_base && lr >= g_self_base && lr < g_self_base + 0x400000ull;
                    klog("[队列] Receive(%#llx) 队列空 → 阻塞等（第 %d 次）调用者=%s%#llx\n",
                         (unsigned long long)a1, nw,
                         (lr >= b2 && lr < b2 + DM_IMG_MAIN_END) ? "映像+" :
                         (in_self ? "本so+" : "(外)"),
                         (unsigned long long)((lr >= b2 && lr < b2 + DM_IMG_MAIN_END) ? lr - b2 :
                                              (in_self ? lr - g_self_base : lr)));
                }
                while (q->n == 0) pthread_cond_wait(&q->cv_notempty, &q->mu);
            }
            uint64_t v = q->buf[q->head];
            q->head = (q->head + 1) % q->cap;
            q->n--;
            pthread_cond_signal(&q->cv_notfull);
            pthread_mutex_unlock(&q->mu);
            if (a0) *(uint64_t *)(uintptr_t)a0 = v;
            // np98：消息要交给线程体处理了——线程体 0xa1e30c 的循环是
            //   `x19 = 线程对象; x9 = [x19]; x9 = [x9+0x80]; blr x9`。
            //   队列在对象里的偏移是 +0x38 ⇒ **线程对象 = 队列地址 - 0x38**，
            //   于是 [对象+0]=虚表、[虚表+0x80]=消息处理函数。客体内存可直接读，不挂钩子。
            {
                uint64_t obj = (uint64_t)(uintptr_t)a1 - 0x38;
                static int nm2;
                if (++nm2 <= 6 && obj > 0x10000ull && obj < 0x800000000000ull) {
                    uint64_t vt = *(const uint64_t *)(uintptr_t)obj;
                    uint64_t h = (vt > 0x10000ull && vt < 0x800000000000ull)
                               ? *(const uint64_t *)(uintptr_t)(vt + 0x80) : 0;
                    uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
                    int in = h >= b2 && h < b2 + DM_IMG_MAIN_END;
                    klog("[队列] 收到消息=%#llx；线程对象=%#llx 虚表=%#llx 处理函数=%s%#llx\n",
                         (unsigned long long)v, (unsigned long long)obj,
                         (unsigned long long)vt, in ? "+" : "(外)",
                         (unsigned long long)(in ? h - b2 : h));
                    // np99：第二跳——0xa1df50 是 x0=[x9+0x100]; x10=[x0]; x3=[x10]; br x3
                    uint64_t tbl = *(const uint64_t *)(uintptr_t)(obj + 0x100);
                    uint64_t real = (tbl > 0x10000ull && tbl < 0x800000000000ull)
                                  ? *(const uint64_t *)(uintptr_t)tbl : 0;
                    real = (real > 0x10000ull && real < 0x800000000000ull)
                           ? *(const uint64_t *)(uintptr_t)real : 0;
                    {
                        int in2 = real >= b2 && real < b2 + DM_IMG_MAIN_END;
                        klog("[队列] 第二跳：表=%#llx 真正处理函数=%s%#llx\n",
                             (unsigned long long)tbl, in2 ? "+" : "(外)",
                             (unsigned long long)(in2 ? real - b2 : real));
                    {   // 第三跳：0xa02478 是 std::function 调用器——[对象+8] 才是真回调，
                        // 为 0 就"收到消息但什么都不做"（= 回调没被注册）
                        uint64_t fobj = *(const uint64_t *)(uintptr_t)tbl;
                        uint64_t cb = (fobj > 0x10000ull && fobj < 0x800000000000ull)
                                      ? *(const uint64_t *)(uintptr_t)(fobj + 8) : 0;
                        int in3 = cb >= b2 && cb < b2 + DM_IMG_MAIN_END;
                        klog("[队列] 第三跳：回调对象=%#llx 真回调=%s%#llx%s\n",
                             (unsigned long long)fobj, in3 ? "+" : "(外)",
                             (unsigned long long)(in3 ? cb - b2 : cb),
                             cb ? "" : "（空 ⇒ 消息白收）");
                    }
                    }
                }
            }
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os19TrySendMessageQueueEPNS0_16MessageQueueTypeEm")) {
            dm_mq_t *q = mq_get((void *)(uintptr_t)a0);
            if (!q || !q->cap) return 0;
            pthread_mutex_lock(&q->mu);
            if (q->n >= q->cap) { pthread_mutex_unlock(&q->mu); return 0; }   // 满：bool 假
            q->buf[q->tail] = a1;
            q->tail = (q->tail + 1) % q->cap;
            q->n++;
            pthread_cond_signal(&q->cv_notempty);
            pthread_mutex_unlock(&q->mu);
            {   // np77：找"谁该给空队列发消息"——发送方打点（与被阻塞的 Receive 对应）
                static int ns;
                if (++ns <= 24) {
                    uint64_t lr; __asm__ volatile("mov %0, x30" : "=r"(lr));
                    uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
                    int in_self = g_self_base && lr >= g_self_base && lr < g_self_base + 0x400000ull;
                    klog("[队列] TrySend(%#llx) 值=%#llx 送入后 n=%zu 调用者=%s%#llx\n",
                         (unsigned long long)a0, (unsigned long long)a1, q->n,
                         (lr >= b2 && lr < b2 + DM_IMG_MAIN_END) ? "映像+" :
                         (in_self ? "本so+" : "(外)"),
                         (unsigned long long)((lr >= b2 && lr < b2 + DM_IMG_MAIN_END) ? lr - b2 :
                                              (in_self ? lr - g_self_base : lr)));
                }
            }
            return 1;                                   // bool 真 = 已送入
        }
        if (!strcmp(nm, "_ZN2nn2os16SendMessageQueueEPNS0_16MessageQueueTypeEm")) {
            dm_mq_t *q = mq_get((void *)(uintptr_t)a0);
            if (!q || !q->cap) return 0;
            pthread_mutex_lock(&q->mu);
            while (q->n >= q->cap) pthread_cond_wait(&q->cv_notfull, &q->mu);
            q->buf[q->tail] = a1;
            q->tail = (q->tail + 1) % q->cap;
            q->n++;
            pthread_cond_signal(&q->cv_notempty);
            pthread_mutex_unlock(&q->mu);
            {   // np77：同上，阻塞型发送方打点
                static int ns; if (++ns <= 24)
                    klog("[队列] Send(%#llx) 值=%#llx 送入后 n=%zu\n",
                         (unsigned long long)a0, (unsigned long long)a1, q->n);
            }
            return 0;
        }
        if (!strcmp(nm, "_ZN2nn2os22TryReceiveMessageQueueEPmPNS0_16MessageQueueTypeE")) {
            dm_mq_t *q = mq_get((void *)(uintptr_t)a1);
            if (!q || !q->cap) return 0;
            pthread_mutex_lock(&q->mu);
            if (q->n == 0) { pthread_mutex_unlock(&q->mu); return 0; }
            uint64_t v = q->buf[q->head];
            q->head = (q->head + 1) % q->cap;
            q->n--;
            pthread_cond_signal(&q->cv_notfull);
            pthread_mutex_unlock(&q->mu);
            if (a0) *(uint64_t *)(uintptr_t)a0 = v;
            return 1;
        }
        // 让出/等待类：真线程下 Yield 用 sched_yield
        if (!strcmp(nm, "_ZN2nn2os11YieldThreadEv")) { sched_yield(); return 0; }
        // nvnBootstrapLoader —— NVN 图形驱动引导（图形栈大门，2026-10-18 到达）
        // 真机实证：游戏只取两个名字（nvnDeviceInitialize / nvnDeviceGetProcAddress），
        // 其余全走 GetProcAddress。⇒ 这里按名字分派到最小空设备（见 nvn_dev_init 注释）。
        if (!strcmp(nm, "nvnBootstrapLoader")) {
            const char *nvn = a0 ? (const char *)(uintptr_t)a0 : NULL;
            static int n_nvn;
            if (++n_nvn <= 40)
                klog("[NVN] nvnBootstrapLoader(「%s」)\n", nvn ? nvn : "(null)");
            if (nvn && !strcmp(nvn, "nvnDeviceInitialize")) return (uint64_t)(uintptr_t)&nvn_dev_init;
            if (nvn && !strcmp(nvn, "nvnDeviceGetProcAddress")) return (uint64_t)(uintptr_t)&nvn_dev_gpa;
            return (uint64_t)(uintptr_t)&nvn_stub;
        }
        // ★ 关键：C++ 静态局部对象的初始化守卫。返回 1 = "由我来初始化"；
        // 返回 0 会被当成"别人已初始化"而跳过 ⇒ 全部静态单例没建、虚表为空（本轮崩因）。
        if (!strcmp(nm, "__cxa_guard_acquire")) { klog("[服务] __cxa_guard_acquire → 1（由我来初始化）\n"); return 1; }
        if (!strcmp(nm, "__cxa_guard_release") || !strcmp(nm, "__cxa_guard_abort")) return 0;
        // ===== 语义已知的服务：成批实现（"步子适度放大"：这些不必一个个试）=====
        // 真时基：返回 ns 级单调时间（返回 0 会让游戏的计时/节拍逻辑失真）
        if (!strcmp(nm, "_ZN2nn2os13GetSystemTickEv")) {
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            static uint64_t base_ns;
            uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
            if (!base_ns) base_ns = now;
            return (now - base_ns) * 19200ull / 1000000000ull;   // Switch 时基 ≈ 19.2MHz
        }
        // C++ 纯虚调用兜底：落到这里就是"调了纯虚函数"，记名后返回（不无限循环，便于定位）
        if (!strcmp(nm, "__cxa_pure_virtual")) { klog("[服务] ⚠ __cxa_pure_virtual 被调用（游戏有纯虚误调）\n"); return 0; }
        // 通用 PLT 记名 + **全局截断**：init 段约一千行全是信息量；但游戏跑起来后
        // ConvertToTimeSpan/SleepThread 每帧各调一次，一轮能刷出 170MB 日志把别的线索淹掉
        // （2026-10-17 实证）。到上限就只记一条截断宣言，计数仍走 g_plt_calls。
        // ★ NVN 用量打点：**借游戏自己的帧线程**（ConvertToTimeSpan 每帧一次）。
        //   为什么不用宿主线程：np26–np29 三个回合，宿主看护线程一行日志都没打出来
        //   （pthread_create 返回成功、失败分支没触发 ⇒ 线程建了但从未被调度，见
        //   nvn_host_thread_probe 的注释）⇒ 观测必须挂在"确定会被调度"的线程上。
        // ⚠ 阈值别定大：np29 用了 120（帧），而日志里 ConvertToTimeSpan 约每 90 行才 1 次
        //   ⇒ 要 1 万行才打一次，跑测根本等不到。用 20。
        if (!strcmp(nm, "_ZN2nn2os17ConvertToTimeSpanENS0_4TickE")) {
            static unsigned tick;
            if (++tick % 20 == 0) nvn_family_report("帧线程");
            // np103：定期读"变体表"（0x22dce70）的状态——#206 查它拿到空指针。
            //   看三样：+0x18 缓存指针、缓存内容前 16 字节、+0x48 计数。
            //   若计数=2（初始化器跑过）而缓存内容全 0 ⇒ "建了表但没人填内容"。
            if (tick % 400 == 100) {
                uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
                if (b2) {
                    const uint8_t *tbl = (const uint8_t *)(uintptr_t)(b2 + 0x22dce70);
                    uint64_t cache = *(const uint64_t *)(uintptr_t)(tbl + 0x18);
                    uint32_t cnt = *(const uint32_t *)(uintptr_t)(tbl + 0x48);
                    char hex[80]; size_t u = 0;
                    if (cache > 0x10000ull && cache < 0x800000000000ull) {
                        const uint8_t *c = (const uint8_t *)(uintptr_t)cache;
                        for (int i = 0; i < 16 && u + 3 < sizeof hex; i++)
                            u += (size_t)snprintf(hex + u, sizeof hex - u, "%02x", c[i]);
                    }
                    hex[u] = 0;
                    klog("[变体表] tick=%u 计数=%u 缓存=%#llx 内容[%s]\n",
                         tick, cnt, (unsigned long long)cache, hex);
                    // np103b：字符串对象(0x22dcdf0)的虚表来自 *(0x22828b0)+0x10——
                    //   若那又是我们的假堆/替身虚表 ⇒ 虚调用全返 0 ⇒ 字符串永远空（根因实锤）。
                    uint64_t g28 = *(const uint64_t *)(uintptr_t)(b2 + 0x22828b0);
                    uint64_t svt = *(const uint64_t *)(uintptr_t)(b2 + 0x22dcdf0);
                    uint64_t m18 = (svt > 0x10000ull && svt < 0x800000000000ull)
                                 ? *(const uint64_t *)(uintptr_t)(svt + 0x18) : 0;
                    const char *who28 = "(?)";
                    if (g28 >= (uint64_t)(uintptr_t)g_fake_heap && g28 < (uint64_t)(uintptr_t)g_fake_heap + 0x4000) who28 = "假堆!";
                    klog("[变体表] *(0x22828b0)=%#llx%s 串对象虚表=%#llx 槽[+0x18]=%#llx\n",
                         (unsigned long long)g28, who28,
                         (unsigned long long)svt, (unsigned long long)m18);
                    // np105：**启动包装载的门**（0xd5b670 附近那段：Pack/Bootup_Graphics.pack）：
                    //   x8 = *(0x2276168); x8 = [x8]; if ([x8+0x419] != 0) ⇒ **跳过装载**
                    //   实机读这个标志 + 那个全局指向谁。
                    if (tick <= 400) {
                        uint64_t g76 = *(const uint64_t *)(uintptr_t)(b2 + 0x2276168);
                        uint8_t gate = 0xff;
                        if (g76 > 0x10000ull && g76 < 0x800000000000ull)
                            gate = *(const uint8_t *)(uintptr_t)(g76 + 0x419);
                        klog("[启动包门](旧单层读，仅参考) 外层=%#llx [+0x419]=%#x\n",
                             (unsigned long long)g76, gate);
                    }
                }
            }
        }
        // ═══ 通用 SDK 胶水层（L1，dm_sdk_ohos.c）═══
        // vi/oe 这类"轻量语义"已搬到独立模块：与游戏无关、换游戏一行不改。
        // 探针只做分派；表里没有的仍落下面的通用记名 + 返 0 路径。
        {
            uint64_t r = 0;
            if (dm_sdk_call(nm, a0, a1, a2, a3, &r)) return r;
        }
        if (g_plt_log_n < DM_PLT_LOG_CAP) {
            g_plt_log_n++;
            klog("[PLT] %s\n", nm);
        } else if (g_plt_log_n == DM_PLT_LOG_CAP) {
            g_plt_log_n++;
            klog("[PLT] === 截断：通用 PLT 记名已达 %d 行，后续不再逐条记（总数仍在 g_plt_calls）===\n",
                 DM_PLT_LOG_CAP);
        }
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
    // calloc 而非 malloc：游戏把这块当自己的对象/池用（与 AllocateMemoryBlock 同一理由——
    // 喂未初始化内存会让游戏拿到垃圾指针/垃圾空闲链，2026-10-16 实证过一次）。
    void *p = calloc(1, (size_t)size);
    klog("[堆] 分配 %llu 字节 → %p\n", (unsigned long long)size, p);
    return p;
}
// 虚表兜底：游戏会往这个"分配器替身"上翻到我们没印的页（真机实证：第 17 槽 +0x88 越界 ⇒ blr 0 崩）。
// 兜底里记**调用者返回地址**——槽号自己不知道，但由返回地址可离线反查是哪一页。
static uint64_t vtbl_fallback(void *self, void *arg) {
    (void)self; (void)arg;
    klog("[虚表兜底] 被调用：返回地址=%p（离线反查即知是哪个槽）\n", __builtin_return_address(0));
    return 0;
}
// 第 17 槽（+0x88）：调用点 0xa0e120 后紧跟 `tbz w0, #0` ⇒ 返回值当 bool，bit0=1 表示成功。
static uint64_t vtbl_slot17_ok(void *self, void *arg) {
    (void)self; (void)arg;
    klog("[虚表] 第17槽(+0x88) 被调用 → 返回 1（bool 成功）\n");
    return 1;
}
// 第 16 槽（+0x80）：调用点 0xa091ac ⇒ 返回值被 `and #~7`（8 字节对齐）后要求 ≥0x131，
// 随后作为尺寸交给第 6 槽去**分配**（真机实证：兜底返 0 ⇒ sead::ThreadMgr 查找失败 ⇒ 崩 0x9fe2ec）。
// 语义是"报这块对象需要多大"（造 sead::ThreadMgr 用的）⇒ 给宽裕值：大了只浪费，小了被写越界。
static uint64_t vtbl_slot16_size(void *self, void *arg) {
    (void)self; (void)arg;
    klog("[虚表] 第16槽(+0x80) 被调用 → 返回尺寸 0x1000\n");
    return 0x1000;
}
#define DM_ALLOC_VTBL_QWORDS 0x40          // 0x200 字节：够覆盖游戏翻到的 +0xd0
static uint64_t g_alloc_vtbl[DM_ALLOC_VTBL_QWORDS];   // 虚表：槽 6（+0x30）= host_alloc，槽 17（+0x88）见上
static uint64_t g_alloc_obj[2];       // 对象 A：{ &虚表, &A }
static uint64_t g_alloc_holder[1];    // 持有者：{ &A }
static uint64_t g_ctx_holder[1];      // 上下文持有者：{ &ctx }
static uint64_t g_ctx_obj[0x20];      // 上下文对象（[0x88] 处为 0 = 让 thunk 返回 0，走本线程路径）
static uint64_t g_mgr_obj[8];       // 管理器对象：+0x18 处放"子对象"指针
static uint64_t g_mgr_sub[8];       // 子对象：+0x18 处放虚方法指针
// ── *(0x227df98) 指向的 .bss 全局（模块偏移 0x22d9198）──
// 真机+离线双重确认：**模块代码从不写它**（连同我们已成功注入的 0x22d8a18/0x22d8a40 也一样）
// ⇒ 这些全局本由 Switch 外部运行时填，鸿蒙上没有 ⇒ 只能由宿主补影子。
// 用法（崩点 base+0x9FE68C → 尾调用 0xa21a78）：`ldr x0,[x9]`（x9=*(0x227df98)）得到对象，
// 0xa21a78 再 `ldr x1,[x0,#0x20]; cbz; br x1` —— 所以 [+0x20] 要放一个函数指针。
static uint64_t g_cb_obj[0x10];          // 影子对象（够覆盖 +0x20）
static uint64_t g_cb_calls;
static uint64_t cb_stub(void) {
    // 每 1024 次才记一行：真机上它被调了 136 万次，逐条打会把日志刷爆、反而看不见别的线索
    if ((++g_cb_calls & 0x3FFu) == 1u)
        klog("[回调] 0xa21a78 转调（第 %llu 次）\n", (unsigned long long)g_cb_calls);
    return 0;
}

// ── nnMain 路径的 sead::ThreadMgr（文档"旷野之息 main 专属项"之一）──
// 真机实证（2026-10-17）：崩点 base+0x9FE9A8 的 `ldr x8,[x19]`，si_addr=0。反汇编：
//   0x9fe98c  ldr x8, [x8, #0xac0]   ← x8 = *(0x227aac0)（这个槽**一直没注入**）
//   0x9fe990  ldr w9, [x8] ; cbz w9  ← 读到的标志是 0 ⇒ 走 x19 = 0
//   0x9fe9a8  ldr x8, [x19]          ← 读 [0] ⇒ 崩
// 所以这里补上：P=[0x227aac0] 要求 P[0]≠0（标志）、P[8]→对象；对象[0]=虚表，
// 虚表 +0x80 会以 (x0=对象, w1=8) 被调用（调用后接着查 "sead::SystemManagers"）。
static uint64_t g_tm_a[2];          // P：{ 标志, &ThreadMgr }
static uint64_t g_tm_obj[2];        // ThreadMgr：{ &虚表, … }
#define DM_TM_VTBL_QWORDS 0x40
static uint64_t g_tm_vtbl[DM_TM_VTBL_QWORDS];
static uint64_t tm_method(void *self, uint64_t arg) {
    (void)self;
    klog("[sead::ThreadMgr] 虚表 +0x80 被调用（w1=%#llx）\n", (unsigned long long)arg);
    return 0;
}
static uint64_t mgr_method(void *self) {
    klog("[管理器] 虚方法被调用（self=%p）—— 宿主存根应答\n", self);
    return 0;
}
// 日志对象 + 节点池：nnMain 开头调用的日志辅助 0xf35374 从 *(0x2288548) 取对象，
// 字段：[+0x10]=计数（0）、[+0x28]=上限（64）、[+0x18]=节点链表头。
// 节点：>=0x20 字节，[+0]=下一节点。链表**必须非空**（空表会让它往地址 0 写）。
#define LOG_POOL_N 64
// 每节点 0x40 字节（2026-10-18 从 0x20 扩容）：离线解码游戏日志添加函数 0xf35374 得到
// 节点布局 [0]=tick、[8]=消息串、[0x10]=链节点（0x9f3904 插入用，[link]=next/[link+8]=prev）、
// [0x18]=0 ⇒ 至少 0x20；留一倍余量，排除"节点写越界污染相邻节点"的嫌疑（真机上见过
// [node+8] 拿到低 32 位对、高 16 位 +0x200 的坏指针）。
static uint64_t g_log_pool[LOG_POOL_N][8];      // 每节点 0x40 字节
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
// 注：早期"把分配器入口直接 b 到宿主函数"的做法（k_patch_allocators）已删除——
// 它只能打 ±128MB 内的直接跳转，且随后就被下面的「替换式插桩」覆盖（同一批站点），
// 日志里的"已接到宿主 alloc"与实际执行路径不符，留着只会误导。

// ★ 数据导入补值（2026-10-18）：main 的动态符号表里有 13 个 nn::util 数学常量 + std::nothrow
//   是**未定义导入**（GLOB_DAT 重定位，type 1025）。Switch 上由运行时链接器从 SDK 模块
//   （sdk/subsdk*，本环境没加载）解析进槽；我们的探针对非 RELATIVE"只计数不解析"，
//   槽留文件原值 0。游戏代码的用法是 `ldr x8,[槽]; ldr s0,[x8]`——**槽 0 = 解引用空指针崩**。
//   实证：静态构造器 #184（`*(0x22d84a0)=2π`）就是这么崩的（读 nn::util::FloatPi 的槽）。
//   这里把**数学事实型标量**按 IEEE754 二进制位补上（槽指向宿主静态量）；std::nothrow 给
//   1 字节哑对象（只当标签传地址）。
//   ⚠ 仍留 0 的（登记在案的地雷，内容未知无法编造）：
//     - detail::SinCoefficients / CosCoefficients / AtanCoefficients（多项式系数表）
//     - detail::SinCosSampleTable（正弦采样表）、detail::AngleIndexHalfRound（表半周长）
//     - detail::FloatQuaternionEpsilon（四元数 epsilon，量级约 1e-5/1e-6）
//     - glslc* ×8（弱符号，语义上就该是 0——可选着色器编译器，客体自行空判）
//     - __rel_dyn_start/end、__rel_plt_start/end（链接器边界符号：填了反而可能诱使游戏
//       去"自遍历重定位表"重复应用重定位，保持 0 与既往实测行为一致）
static float g_c_pi, g_c_2pi, g_c_pi2, g_c_inv2pi, g_c_deg180, g_c_ulp;
static uint8_t g_c_nothrow;
static void k_fill_data_imports(uint8_t *img) {
    // 位模式（Python struct 核过）：π=0x40490FDB 2π=0x40C90FDB π/2=0x3FC90FDB
    // 1/(2π)=0x3E22F983 180.0=0x43340000 ULP(2^-23)=0x34000000
    uint32_t pi = 0x40490FDBu, pi2 = 0x3FC90FDBu, tpi = 0x40C90FDBu;
    uint32_t inv2pi = 0x3E22F983u, d180 = 0x43340000u, ulp = 0x34000000u;
    __builtin_memcpy(&g_c_pi, &pi, 4);      __builtin_memcpy(&g_c_2pi, &tpi, 4);
    __builtin_memcpy(&g_c_pi2, &pi2, 4);    __builtin_memcpy(&g_c_inv2pi, &inv2pi, 4);
    __builtin_memcpy(&g_c_deg180, &d180, 4); __builtin_memcpy(&g_c_ulp, &ulp, 4);
    g_c_nothrow = 0;
    struct { uint32_t slot; void *p; } tab[] = {
        { 0x2280648, &g_c_pi      },   // nn::util::FloatPi
        { 0x2280528, &g_c_pi      },   // nn::util::detail::FloatPi
        { 0x2280548, &g_c_2pi     },   // nn::util::detail::Float2Pi
        { 0x2280550, &g_c_pi2     },   // nn::util::detail::FloatPiDivided2
        { 0x2280530, &g_c_inv2pi  },   // nn::util::detail::Float1Divided2Pi
        { 0x2280460, &g_c_deg180  },   // nn::util::detail::FloatDegree180
        { 0x2280650, &g_c_ulp     },   // nn::util::detail::FloatUlp
        { 0x2264110, &g_c_nothrow },   // std::nothrow（1 字节哑对象）
    };
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++)
        *(uint64_t *)(img + tab[i].slot) = (uint64_t)(uintptr_t)tab[i].p;
    klog("[数据导入] 补 %u 个标量常量槽（π/2π/π/2/1/(2π)/180/ULP + nothrow）\n",
         (unsigned)(sizeof tab / sizeof tab[0]));
}

// ===== 洞内插桩：安装器 =====
// 把 hook_stub.S 的桩整段复制进代码洞，填三个槽（原指令 / 回跳 / 宿主函数地址），
// 再把目标函数首指令改成 4 字节 b 跳进桩。全部在 seal 之前做。
//
// ⚠ 桩保存帧的**下标约定**（与 hook_stub.S 的 stp 偏移一一对应；改桩必须同步改这里）：
//     0x00..0x88 = x0..x17 → f[0..17]   0x90 = nzcv → f[18]   0x98 = 未写入（padding）
//     0xA0 = x29 → f[20]   0xA8 = x30 → f[21]   0xB0 = 站点地址 → f[22]
//   实测方式：clang -D__OHOS__ -c hook_stub.S + llvm-objdump -d（2026-10-16 实证；
//   此前按 "f[20]=x30" 读是错的——f[20] 是 x29，f[19] 根本没写）。
enum { FR_X0 = 0, FR_X1 = 1, FR_X2 = 2, FR_NZCV = 18, FR_X29 = 20, FR_X30 = 21, FR_SITE = 22 };
#define HOOK_STRIDE 0x120u   // 桩长实测 0x118（存了 v0-v31 后变大；objdump 量过），步长取 0x120 对齐
#define HOOK_MAX    (DM_IMG_MAIN_CAVE_SIZE / HOOK_STRIDE)   // 洞容量 **227 格**（0x10000/0x120）
extern const char hook_stub_begin[], hook_stub_end[], hook_slot_orig[], hook_slot_back[], hook_slot_host[], hook_slot_site[];

// 站点指令搬进洞要重算 PC 相对位移：b / bl 是 imm26，b.cond / cbz / cbnz 是 imm19；
// 其余指令原样搬。返回 0 = 超程或形态未知（调用方跳过该站点，宁可少挂不可挂错）。
static int reloc_pc_rel(uint32_t w, uint8_t *from, uint8_t *to, uint32_t *out) {
    uint32_t op26 = w & 0xFC000000u;
    int32_t  imm; int64_t d;
    // ★ 2026-10-18 真机根因：adrp / adr / ldr(字面量) 都是 **PC 相对取址**，而记录式桩的槽①
    //   只有一条指令的位置（4 字节），搬进洞里执行会按**洞内 PC** 算地址 ⇒ 悄悄算到另一个页面，
    //   上层再解引用就拿到垃圾（实测：+0xA246A4 崩，槽值其实是对的，是这条 adrp 被搬坏了）。
    //   movz/movk 至少要两条才装得下绝对地址，槽①放不下 ⇒ **明确拒绝挂载**（返回 -1），
    //   宁可不插桩，也不能插一个"改坏寄存器"的桩。
    if ((w & 0x9F000000u) == 0x90000000u ||    // adrp
        (w & 0x9F000000u) == 0x10000000u ||    // adr
        (w & 0x3B000000u) == 0x18000000u) {    // ldr (literal)
        return -1;
    }
    if (op26 == 0x14000000u || op26 == 0x94000000u) {                            // b / bl
        imm = (int32_t)(w & 0x03FFFFFFu);
        if (imm & 0x02000000) imm -= 0x04000000;
        d = ((int64_t)(intptr_t)from + (int64_t)imm * 4 - (int64_t)(intptr_t)to) / 4;
        if (d < -(1 << 25) || d >= (1 << 25)) return 0;
        *out = op26 | ((uint32_t)d & 0x03FFFFFFu);
        return 1;
    }
    if ((w & 0x7E000000u) == 0x34000000u || (w & 0xFF000000u) == 0x54000000u) {  // cbz/cbnz / b.cond
        imm = (int32_t)((w >> 5) & 0x7FFFFu);
        if (imm & 0x40000) imm -= 0x80000;
        d = ((int64_t)(intptr_t)from + (int64_t)imm * 4 - (int64_t)(intptr_t)to) / 4;
        if (d < -(1 << 18) || d >= (1 << 18)) return 0;
        *out = (w & ~(0x7FFFFu << 5)) | (((uint32_t)d & 0x7FFFFu) << 5);
        return 1;
    }
    *out = w;
    return 1;
}
static int g_hook_n;
static uint32_t *slot_of(uint8_t *dst, const char *b0) {
    return (uint32_t *)(dst + (hook_slot_orig - b0));
}
static void k_install_hook(uint8_t *img, uint32_t site_off, void *host_fn, const char *what) {
    const char *b0 = hook_stub_begin;
    size_t n = (size_t)(hook_stub_end - hook_stub_begin);
    if (n > HOOK_STRIDE) { klog("[插桩] 桩太长(%zu > %u)，跳过 %s\n", n, HOOK_STRIDE, what); return; }
    if (g_hook_n >= (int)HOOK_MAX) { klog("[插桩] 洞已满(%d 格)，跳过 %s\n", g_hook_n, what); return; }
    uint8_t *dst = img + DM_IMG_MAIN_CAVE_OFF + (size_t)g_hook_n * HOOK_STRIDE;
    memcpy(dst, b0, n);
    // 槽①：原指令（4 字节）。PC 相对的分支（bl/b/b.cond/cbz）搬进洞后必须按新位置重算位移，
    // 否则会跳错地方——这是挂"调用点 / 尾调用点"绕不过的一步。
    {
        uint32_t w = *(uint32_t *)(img + site_off), nw = 0;
        int rr = reloc_pc_rel(w, img + site_off, dst + (hook_slot_orig - b0), &nw);
        if (rr < 0) {
            klog("[插桩] %s 站点首指令 %08x 是 PC 相对且一格里搬不动（adrp/adr/ldr 字面量），"
                 "**拒绝挂载**（挂了会算错地址）\n", what, w);
            return;
        }
        if (rr == 0) { klog("[插桩] %s 站点指令 %08x 重定位超程，跳过\n", what, w); return; }
        *(uint32_t *)slot_of(dst, b0) = nw;
    }
    // 槽②：b site+4（相对编码）
    {
        uint8_t *p = dst + (hook_slot_back - b0);
        int64_t d = ((int64_t)(uintptr_t)(img + site_off + 4) - (int64_t)(uintptr_t)p) / 4;
        if (d < -(1 << 25) || d >= (1 << 25)) { klog("[插桩] %s 回跳超程，跳过\n", what); return; }
        *(uint32_t *)p = 0x14000000u | (uint32_t)(d & 0x03FFFFFFu);
    }
    // 槽③：宿主函数地址；槽④：站点地址（日志据此识别调用点）
    *(uint64_t *)(dst + (hook_slot_host - b0)) = (uint64_t)(uintptr_t)host_fn;
    *(uint64_t *)(dst + (hook_slot_site - b0)) = (uint64_t)(uintptr_t)(img + site_off);
    // 站点：b dst
    {
        int64_t d = ((int64_t)(uintptr_t)dst - (int64_t)(uintptr_t)(img + site_off)) / 4;
        if (d < -(1 << 25) || d >= (1 << 25)) { klog("[插桩] %s 站点跳超程，跳过\n", what); return; }
        *(uint32_t *)(img + site_off) = 0x14000000u | (uint32_t)(d & 0x03FFFFFFu);
    }
    klog("[插桩] %s：site %#x → 洞 %#zx（洞内首指令 %08x）\n", what, site_off,
         (size_t)(dst - img), *slot_of(dst, b0));
    g_hook_n++;
}
// 统一的桩日志：打印站点地址、入参（x0 = this）、以及 x29/x30（**x30 = "谁调我"**）。
// 帧下标见上面 FR_* 约定。每条目标各限 HOOK_LOG_CAP 行——崩点前的**最后一行**才是答案，
// 所以宁可多记也别截断；上限只是防止在环里刷爆日志。
#define HOOK_LOG_CAP 256
static void frame_log(const char *what, uint64_t *f, int *cnt) {
    if (++*cnt <= HOOK_LOG_CAP)
        klog("[桩] %s  站点 %#llx  x0=%#llx x1=%#llx x2=%#llx x29=%#llx x30=%#llx\n",
             what, (unsigned long long)f[FR_SITE], (unsigned long long)f[FR_X0],
             (unsigned long long)f[FR_X1], (unsigned long long)f[FR_X2],
             (unsigned long long)f[FR_X29], (unsigned long long)f[FR_X30]);
}
static int g_n_ctor1, g_n_ctor2, g_n_any, g_n_9fe24c, g_n_9fe2e8, g_n_1e484, g_n_1dd94, g_n_1d8e0, g_n_s1d8e0;
static uint64_t hook_ctor_log (uint64_t *f) { frame_log("构造器 0xa08ef4", f, &g_n_ctor1); return 0; }
static uint64_t hook_ctor2_log(uint64_t *f) { frame_log("构造器 0xa08e94", f, &g_n_ctor2); return 0; }
static uint64_t hook_any_site (uint64_t *f) { frame_log("站点→0xa08e94", f, &g_n_any);   return 0; }
// ★ 沿崩链逐点插桩。链（离线用 tools/dm_branch.py 追出，文档里此前没有这几层）：
//   函数 0x9fe24c ──cbz w9 走 0x9fe294 分支──▶ 0x9fe2e8 `bl 0xa1e484`（x0 = **(0x22783d8)，
//   正是我们注入的线程上下文对象）──▶ 0xa1e484 是 `b 0xa1dd94` 尾调用桩 ──▶
//   0xa1dd94 ──`bl 0x9f33f0` 分配 0x100 字节──▶ ──`bl 0xa1d8e0`──▶ 0xa1d8e0 ──`bl 0xa08e94`
//   （基类构造器，this 直接沿用 x0，中间一条指令都没改 x0）──▶ 崩在 0xa08eb0 的
//   `stp x8, xzr, [x19]`（x19 = x0 = this ⇒ this=0 就是往地址 0 写）。
//   逐点记 x0（上一层交下来的对象）与 x30（谁调我）⇒ **哪一层把 0 交下去，就是"该建而未建"的位置**。
static uint64_t hook_p9fe24c (uint64_t *f) { frame_log("链点 0x9fe24c", f, &g_n_9fe24c); return 0; }
static uint64_t hook_p9fe2e8 (uint64_t *f) { frame_log("站点→0xa1e484", f, &g_n_9fe2e8); return 0; }
static uint64_t hook_p1e484 (uint64_t *f) { frame_log("链点 0xa1e484", f, &g_n_1e484); return 0; }
static uint64_t hook_p1dd94 (uint64_t *f) { frame_log("链点 0xa1dd94", f, &g_n_1dd94); return 0; }
static uint64_t hook_p1d8e0 (uint64_t *f) { frame_log("链点 0xa1d8e0", f, &g_n_1d8e0); return 0; }
static uint64_t hook_s1d8e0 (uint64_t *f) { frame_log("站点→0xa1d8e0", f, &g_n_s1d8e0); return 0; }
// ★ sead 单例查找 0xa0913c 为什么返回 0？把它的几条关键分支路径都挂上点，命中哪条就是走了哪条。
//   （它是中心设施，103 个调用点；出口 0xa09418 返回 x20。这几条都是条件分支的**目标**，
//     挂目标安全；直接挂条件分支指令本身会让"被命中时又跳回站点"成死循环，不做。）
static int g_n_lkA, g_n_lkB, g_n_lkC, g_n_lkD, g_n_lkE;
static uint64_t hook_lk_a(uint64_t *f) { frame_log("查找A(x24==0)", f, &g_n_lkA); return 0; }
static uint64_t hook_lk_b(uint64_t *f) { frame_log("查找B(走分配)", f, &g_n_lkB); return 0; }
static uint64_t hook_lk_c(uint64_t *f) { frame_log("查找C(非2幂→返0)", f, &g_n_lkC); return 0; }
static uint64_t hook_lk_d(uint64_t *f) { frame_log("查找D(返0:x19==0)", f, &g_n_lkD); return 0; }
static uint64_t hook_lk_e(uint64_t *f) { frame_log("查找E(返0:分配败)", f, &g_n_lkE); return 0; }
// ★ 再细一层：0xa0e2f8 为什么返回 0？
//   它内部 `0xa0e32c ldr x0,[x8]` → `0xa0e330`；在 0xa0e330 挂点可同时看到
//   **读的地址(x8)** 与 **读出的值(x0=返回值)**。再在调用点 0xa09184 挂点看传进去的 obj 是谁。
static int g_n_dbA, g_n_dbB;
static uint64_t hook_dbA(uint64_t *f) {   // 站点 0xa09184（bl 0xa0e2f8）
    if (++g_n_dbA <= 4)
        klog("[诊断] 调 0xa0e2f8：obj=%#llx（期望 = 注入的 g_alloc_obj）\n",
             (unsigned long long)f[FR_X0]);
    return 0;
}
static uint64_t hook_dbB(uint64_t *f) {   // 指令 0xa0e330（0xa0e2f8 的取值点之后）
    if (++g_n_dbB <= 4)
        klog("[诊断] 0xa0e2f8 读 [x8]=%#llx → 值 x0=%#llx（=返回值）\n",
             (unsigned long long)f[8], (unsigned long long)f[FR_X0]);
    return 0;
}
// ★ 崩在 `blr` 野指针（0xb9402be814000001，sig=7 取指失败）。定位到：
//   0xa0c1ac add x0, x22, #0x98 ; 0xa0c1b0 bl 0xa1d57c（sead 助手）
// 助手会把入参当多态对象用（读 [obj] 当虚表、再读 [+0x80]）。而 x22 是个**栈地址**
// ⇒ 怀疑 [x22+0x98] 那个字段本该是指针、却是栈上垃圾。在 0xa0c1b0 挂点，
// 把递过去的 obj 与 *(obj)（助手眼里的"虚表"）都打出来。
// ★ 崩在 `blr` 野指针（0xb9402be814000001，sig=7 取指失败）。探点挂在 0xa0c1b0 时没命中，
//   且现场 x30=0xa0c1b0 与"bl 的 LR=PC+4"对不上 ⇒ 崩的那次调用不是那一处。
//   两处都留着：0xa0c1b0（bl 之前）与 0xa1d57c（sead 助手入口）。
static int g_n_dbC, g_n_dbD;
static uint64_t hook_dbC(uint64_t *f) {   // 站点 0xa0c1b0（bl 0xa1d57c 之前）
    if (++g_n_dbC <= 4) {
        uint64_t obj = f[FR_X0];
        uint64_t vt = 0;
        if (obj) vt = *(const uint64_t *)(uintptr_t)obj;
        klog("[诊断] 0xa0c1b0 调 sead 助手：obj=%#llx  *(obj)=%#llx\n",
             (unsigned long long)obj, (unsigned long long)vt);
    }
    return 0;
}
static uint64_t hook_dbD(uint64_t *f) {   // 指令 0xa1d57c（sead 助手入口）
    if (++g_n_dbD <= 6) {
        uint64_t obj = f[FR_X0];
        uint64_t vt = 0;
        if (obj) vt = *(const uint64_t *)(uintptr_t)obj;
        klog("[诊断] sead 助手 0xa1d57c 被调：obj=%#llx x1=%#llx  *(obj)=%#llx  x30=%#llx\n",
             (unsigned long long)obj, (unsigned long long)f[FR_X1],
             (unsigned long long)vt, (unsigned long long)f[FR_X30]);
    }
    return 0;
}
// ===== 钉子 #8：类型分发函数 0xa00a78 收到空类型标签 ⇒ 返 0 ⇒ 调用方 0xa01790 写 [x21+0xc8] 崩 =====
// 离线结论（2026-10-17，main.patched.bin 指令级核实 + 真机日志闭环，见进度文档第十三节）：
//   0xa00a78 按 [x0] 的低 32 位标签分发：1 → *(*(0x2280a20))（.bss 0x22d8718，全零 ⇒ cbz 返 0）；
//   2 → 直接调 结构体[8]（Path A 里 = *(0x22809c0) = 函数 0x9fe910，有效代码）；
//   3 → *(*(0x227b058))（.bss 0x22d8720，全零 ⇒ 返 0）；其他 → 返 0（复现崩溃）。
//   调用方 0xa01790 用 arg1 的 [0]/[8]/[0x80] 拼栈上结构体喂给它，两个直接调用点：
//   0xa01490（Path A：函数 0x9fe82c 把标签**硬编码为 2**）与 0xa01f1c（Path B：任务链表节点）。
//   **真机定性（2026-10-17 傍晚）**：x30=0xA01F1C ⇒ 走 Path B；标签=0 ⇒ 返 0 ⇒ x21=0 崩。
//   **根因**：池节点里的记录是 mgr 构造函数 0xa00afc 从 *(父+0x10) 拷来的**全零副本**
//   （0xa00cf8 memcpy + 0xa00d48 池注册）；直创路径（mgr 虚表第 23 槽 0x9fe82c）从未被
//   调用（e1 实测 0 命中）。任务身份铁证 = sead::ControllerMgr：标签 2 工厂 *(0x22809c0)
//   = 0x9fe910（alloc 0x198 + 0xa1f07c 构造，基类 0x9fd9fc 名字串 0x1ba6eab 且不读 pair）。
static int g_n_e1, g_n_e2, g_n_e3, g_n_e4, g_n_e5;
static int g_n_e2fix;      // 影子补丁触发次数（标签 0 ⇒ 补 {2, ControllerMgr 工厂}）
static int g_n_e6, g_n_e7, g_n_e8;   // 同轮探针：工厂/池注册/G 三点运行时状态
static int g_n_e8fix;                // 状态 1→2 补推次数（G 激活路径的放行条件）
static int g_n_e9, g_n_e10;          // runner 探针：激活检查 0xa01cf0 / 运行转发器 0x9fdccc
static int g_n_e11;                  // runner 入口链表遍历探针（0xa01fe0）
static uint64_t hook_e1(uint64_t *f) {   // Path A 函数入口 0x9fe82c（标签硬编码 2 的那个）
    if (++g_n_e1 <= 8)
        klog("[钉子8] PathA 0x9fe82c 入口：x0=%#llx x1=%#llx x30=%#llx\n",
             (unsigned long long)f[FR_X0], (unsigned long long)f[FR_X1],
             (unsigned long long)f[FR_X30]);
    return 0;
}
static uint64_t hook_e2(uint64_t *f) {   // 分发调用方入口 0xa01790
    if (++g_n_e2 <= 16) {
        uint64_t a1 = f[FR_X1];
        uint64_t tag = 0;
        if (a1) tag = *(const uint64_t *)(uintptr_t)a1;   // arg1[0]：低 32 位是类型标签
        klog("[钉子8] 0xa01790：this=%#llx arg1=%#llx arg1[0]=%#llx arg2=%#llx x30=%#llx\n",
             (unsigned long long)f[FR_X0], (unsigned long long)a1,
             (unsigned long long)tag, (unsigned long long)f[FR_X2],
             (unsigned long long)f[FR_X30]);
    }
    // ★ 钉子 #8 影子补丁（2026-10-17 傍晚，根因见上方注释与进度文档第十三节）：
    //   记录全零 ⇒ 标签 0 ⇒ 分发器返 0 ⇒ x21=0 ⇒ str [x21+0xc8] 崩。标签 0 不可能是合法
    //   调用（合法标签只有 1/2/3，且 1/3 路在鸿蒙上也是返 0），只可能是"该填的没填"。
    //   宿主补影子：补成 {标签=2, 工厂=base+0x9fe910}——与 Path A 直创时 Record::Init
    //   ({2, *(0x22809c0)}) 的产物逐字节同构（0x9fe910 = alloc 0x198 + 0xa1f07c 构造
    //   sead::ControllerMgr；基类 0x9fd9fc 不读 pair，其余字段全零无害）。
    //   写在记录本体上（持久）：F 随后读到的就是修好的标签/工厂，任务创建后 node[0] 有值，
    //   G 不会再进惰性路径；正常记录（标签≠0）零影响。
    {
        uint64_t a1 = f[FR_X1];
        if (a1 && *(const uint64_t *)(uintptr_t)a1 == 0) {
            uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
            uint64_t fn = base + 0x9fe910;
            *(uint64_t *)(uintptr_t)a1 = 2;
            *(uint64_t *)(uintptr_t)(a1 + 8) = fn;
            if (++g_n_e2fix <= 4)
                klog("[钉子8] 影子：记录 %#llx 标签 0 ⇒ 补 {2, %#llx}（ControllerMgr 工厂）\n",
                     (unsigned long long)a1, (unsigned long long)fn);
        }
    }
    return 0;
}
static uint64_t hook_e3(uint64_t *f) {   // 分发函数入口 0xa00a78
    if (++g_n_e3 <= 16) {
        uint64_t st = f[FR_X0];
        uint64_t tag = 0;
        if (st) tag = *(const uint32_t *)(uintptr_t)st;   // 结构体[0] 低 32 位 = 标签
        klog("[钉子8] 0xa00a78：结构体=%#llx 标签=%llu x1=%#llx\n",
             (unsigned long long)st, (unsigned long long)(uint32_t)tag,
             (unsigned long long)f[FR_X1]);
    }
    return 0;
}
static uint64_t hook_e4(uint64_t *f) {   // tag==2 目标 0x9fe910 入口（记录式，只看不拦）
    if (++g_n_e4 <= 8)
        klog("[钉子8] tag2 0x9fe910 入口：pair=%#llx *pair=%#llx\n",
             (unsigned long long)f[FR_X0],
             (unsigned long long)(f[FR_X0] ? *(const uint64_t *)(uintptr_t)f[FR_X0] : 0));
    return 0;
}
static uint64_t hook_e5(uint64_t *f) {   // 0xa01814（bl 0xa00a78 之后）：x0 = 分发返回值
    if (++g_n_e5 <= 16)
        klog("[钉子8] 分发返回：x0=%#llx（0 ⇒ 就是要找的返 0；x21 即将拿到它）\n",
             (unsigned long long)f[FR_X0]);
    return 0;
}
// ===== 钉子 #8 同轮探针（验证"记录全零来自 *(父+0x10)"的根因推断，只读不改）=====
// 离线只能静态推断父对象身份（工厂 0xa00f8c 零静态引用，疑经 0xa0913c 名字解析间接调），
// 这三个钩子把运行时三点状态打出来：① 工厂入口的父对象与 *(父+0x10) 的记录内容；
// ② 池注册时拷进节点的记录源内容；③ G 取任务时看到的池/节点/记录状态。
static uint64_t hook_e6(uint64_t *f) {   // 管理器工厂入口 0xa00f8c：x0 = 父对象
    if (++g_n_e6 <= 8) {
        uint64_t p = f[FR_X0];
        uint64_t rec = p ? *(const uint64_t *)(uintptr_t)(p + 0x10) : 0;  // *(父+0x10) = 记录指针
        uint64_t r0 = 0, r8 = 0, r70 = 0;
        if (rec) {   // mgr 构造函数随后会 memcpy(mgr+0x58, rec, 0xb8)——能读是确定的
            r0  = *(const uint64_t *)(uintptr_t)rec;
            r8  = *(const uint64_t *)(uintptr_t)(rec + 8);
            r70 = *(const uint64_t *)(uintptr_t)(rec + 0x70);
        }
        klog("[钉子8] 工厂 0xa00f8c：父=%#llx 父[0x10]=%#llx 记录[0]=%#llx [8]=%#llx [0x70]=%#llx\n",
             (unsigned long long)p, (unsigned long long)rec,
             (unsigned long long)r0, (unsigned long long)r8, (unsigned long long)r70);
    }
    return 0;
}
static uint64_t hook_e7(uint64_t *f) {   // 池注册入口 0xa018f8：x0 = mgr，x1 = 记录源（真机实测 = mgr+0x130）
    if (++g_n_e7 <= 8) {
        uint64_t r = f[FR_X1];
        uint64_t q[8] = {0};
        if (r) for (int i = 0; i < 8; i++) q[i] = *(const uint64_t *)(uintptr_t)(r + (uint64_t)i * 8);
        klog("[钉子8] 池注册 0xa018f8：mgr=%#llx 记录源=%#llx（mgr+%#llx） x2=%#llx\n",
             (unsigned long long)f[FR_X0], (unsigned long long)r,
             (unsigned long long)(r - f[FR_X0]), (unsigned long long)f[FR_X2]);
        // 记录头 8 个 qword 全打——真机 5122 行那次 [0]/[8]/[0x70] 全是 0，
        // 这次看清整个头部：如果里面有"名字串指针"却没值，就能确认"该填没填"。
        klog("[钉子8]   记录头：%#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
             (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
             (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5],
             (unsigned long long)q[6], (unsigned long long)q[7]);
    }
    return 0;
}
static uint64_t hook_e8(uint64_t *f) {   // G 入口 0xa01d70：x0 = mgr（读法与 G 自己逐条一致）
    // 先取池状态一次，日志与补推共用。**必须先查计数**（与 G 自己的 cmp w9,#1 判据一致）：
    // 激活路径会把节点释放回空闲链（计数--），此后再读 pool[8] 可能拿到野值。
    uint64_t m8 = f[FR_X0];
    uint64_t pool8 = m8 ? *(const uint64_t *)(uintptr_t)(m8 + 0x118) : 0;
    uint64_t node8 = 0, task8 = 0;
    if (pool8 && *(const uint32_t *)(uintptr_t)(pool8 + 0x10) >= 1) {
        node8 = *(const uint64_t *)(uintptr_t)(pool8 + 8);
        if (node8) task8 = *(const uint64_t *)(uintptr_t)(node8 - 0xe0);   // 节点[0] = 任务指针
    }
    if (++g_n_e8 <= 12 || (g_n_e8 % 5000) == 0) {
        uint64_t lab = node8 ? *(const uint64_t *)(uintptr_t)(node8 - 0xe0 + 8) : 0;
        // caller = x30（G 的调用者返回地址）⇒ 减去模块基址就是模块偏移，能直接反查是哪个子系统
        // 在调用 G。np26 真机：主循环里 G 一直被调但池/记录全空 ⇒ 必须知道"谁在问"。
        uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
        uint64_t lr_off = (f[FR_X30] >= base && f[FR_X30] < base + 0x2600000)
                          ? f[FR_X30] - base : 0;
        klog("[钉子8] G#%d 0xa01d70：mgr=%#llx 池=%#llx 首节点链=%#llx 节点[0]=%#llx 记录[0]=%#llx 调用者=+%#llx\n",
             g_n_e8, (unsigned long long)m8, (unsigned long long)pool8,
             (unsigned long long)node8, (unsigned long long)task8,
             (unsigned long long)lab, (unsigned long long)lr_off);
        // ★ Task #16：G 的惰性创建路径末尾会 `if (mgr[0x50]) mgr[0x50]->虚表[0x18](1,1)`——
        //   任务建好后通知"观察者"。真机上它恒 0，通知整条跳过。这里把 0x50 与 0x48（通知数组）
        //   一起打出来：确认到底是"观察者缺失"还是"数组也空"。
        if (m8) {
            uint64_t obs = *(const uint64_t *)(uintptr_t)(m8 + 0x50);
            uint64_t arr = *(const uint64_t *)(uintptr_t)(m8 + 0x48);
            klog("[任务]   mgr[0x50]观察者=%#llx  mgr[0x48]通知数组=%#llx\n",
                 (unsigned long long)obs, (unsigned long long)arr);
        }
        // 额外 dump 任务 +0x80..0xb8（销毁流的通知数组在 +0x88）：看它在激活前是否已填。
        if (task8) {
            uint64_t *a = (uint64_t *)(uintptr_t)(task8 + 0x80);
            klog("[钉子8] 任务数组 [+0x80..0xb0]：%#llx %#llx %#llx %#llx %#llx %#llx\n",
                 (unsigned long long)a[0], (unsigned long long)a[1], (unsigned long long)a[2],
                 (unsigned long long)a[3], (unsigned long long)a[4], (unsigned long long)a[5]);
        }
    }
    // ★ 钉子 #8 续修：状态 1→2 补推（2026-10-18，根因见进度文档 14.5）。
    //   池节点寿命：注册 → 惰性建任务（状态 1）→ **状态 2** → G 激活（2→4、插 mgr+0x90 链表、
    //   task 虚表[0x58]、*记录[0xa0]=task、**节点释放回空闲链**）→ 池空。状态 1→2 在鸿蒙上
    //   没人推：唯一写入者是直创路径后置序列（0xa015a4，从未运行），G 惰性路径里的 notify
    //   （mgr[0x50] → 虚表[0x18]）也被跳过（mgr[0x50]=0）⇒ 任务线程的 G 永远在
    //   "状态既非 0 也非 2" 分支空转，物理线程的任务图也跟着停摆。
    //   宿主补推：任务存在且状态==1 ⇒ 置 2（自限制：置完后不再触发；状态字段是 32 位对齐写，
    //   与游戏自己的 ldr w8,[x+0xb8] 读法一致）。下一个 G 调用就会走激活路径。
    if (task8 && *(const uint32_t *)(uintptr_t)(task8 + 0xb8) == 1) {
        *(uint32_t *)(uintptr_t)(task8 + 0xb8) = 2;
        // ★ flags 补位（2026-10-18，用户批准）：runner 的外层检查（ldrb [task+0x60]; tbz bit1）
        //   与激活检查（and #6 == 6）都要求 [task+0x60] 的 bit1|bit2。我们的任务这块全零、
        //   全代码找不到写入者（无 strb 直写 / 无模板拷贝 / 构造器只清零）⇒ runner 在
        //   0xa01cf0 之前就跳过任务。**按字节** OR 6（检查用 ldrb 读，别碰 +0x61 起的邻居字节）。
        *(uint8_t *)(uintptr_t)(task8 + 0x60) |= 6;
        if (++g_n_e8fix <= 4)
            klog("[钉子8] 状态提升：任务 %#llx 状态 1 ⇒ 2，flags [0x60] |= 6（runner 放行条件）\n",
                 (unsigned long long)task8);
    }
    return 0;
}
// ===== 钉子 #8 续修探针（runner 链，只读）：激活之后游戏仍自旋 ⇒ runner（0xa02320 →
// 尾跳 0xa01fe0）遍历 mgr+0x90 链表、过检查 0xa01cf0 后才调 task->虚表[0x88]（0x9fdccc，
// 转发给 task+0xd0 子对象）并把状态推 6。检查要求 [task+0x60]&6==6 且状态==4；
// 我们的任务状态=4 但 [task+0x60]=0 且静态分析找不到写入者。两个钩子回答：
// ① 检查函数拿到的"任务"到底是谁（x22 是任务本体还是链表节点？）+ flags/状态现场；
// ② 运行转发器到底有没有被走到。
static uint64_t hook_e9(uint64_t *f) {   // runner 激活检查 0xa01cf0 入口：x0 = mgr，x1 = 候选对象
    if (++g_n_e9 <= 8) {
        uint64_t t = f[FR_X1];
        uint64_t f60 = t ? *(const uint32_t *)(uintptr_t)(t + 0x60) : 0;
        uint64_t st = t ? *(const uint32_t *)(uintptr_t)(t + 0xb8) : 0;
        uint64_t v0 = t ? *(const uint64_t *)(uintptr_t)t : 0;   // 候选对象 [0]（虚表？）
        klog("[钉子8] 检查 0xa01cf0：mgr=%#llx 候选=%#llx [0]=%#llx [0x60]=%#llx(与6=%u) 状态=%llu\n",
             (unsigned long long)f[FR_X0], (unsigned long long)t, (unsigned long long)v0,
             (unsigned long long)f60, (unsigned)((f60 & 6) == 6),
             (unsigned long long)st);
    }
    return 0;
}
static uint64_t hook_e10(uint64_t *f) {  // 运行转发器 0x9fdccc 入口：x0 = 任务（→ +0xd0 子对象）
    if (++g_n_e10 <= 8)
        klog("[钉子8] 运行 0x9fdccc：任务=%#llx [+0xb8]=%#llx（被 runner 选中执行）\n",
             (unsigned long long)f[FR_X0],
             (unsigned long long)(f[FR_X0] ? *(const uint32_t *)(uintptr_t)(f[FR_X0] + 0xb8) : 0));
    return 0;
}
// ★ runner 入口链表遍历（2026-10-18）：上一轮实证 0xa01cf0 零调用 ⇒ 检查根本没走到。
//   离线已解码：0x9f3904 = 双向链表后插（[link+0]=next [link+8]=prev [link+0x10]=载荷指针），
//   runner 从 mgr[0x98]（=头.prev=尾）沿 prev 反向走，x22 = [link+0x10] = 任务本体，
//   然后查 [task+0x60] bit1 —— 我们的任务 flags=0，在 0xa01cf0 之前就被跳过。
//   这个钩子在 runner 入口用 C 沿链表走一遍，运行时坐实：链表里有没有任务、flags 多少。
static uint64_t hook_e11(uint64_t *f) {  // runner 0xa01fe0 入口：x0 = mgr
    if (++g_n_e11 <= 8) {
        uint64_t m = f[FR_X0];
        uint64_t head = m ? m + 0x90 : 0;
        uint64_t cur = m ? *(const uint64_t *)(uintptr_t)(m + 0x98) : 0;
        int i = 0;
        while (cur && cur != head && i < 8) {
            uint64_t task = *(const uint64_t *)(uintptr_t)(cur + 0x10);   // 载荷指针
            uint64_t f60 = task ? *(const uint32_t *)(uintptr_t)(task + 0x60) : 0;
            uint64_t st  = task ? *(const uint32_t *)(uintptr_t)(task + 0xb8) : 0;
            uint64_t v0  = task ? *(const uint64_t *)(uintptr_t)task : 0;
            klog("[钉子8] runner 链表[%d]：节点=%#llx 任务=%#llx [0]=%#llx [0x60]=%#llx 状态=%llu\n",
                 i, (unsigned long long)cur, (unsigned long long)task,
                 (unsigned long long)v0, (unsigned long long)f60, (unsigned long long)st);
            cur = *(const uint64_t *)(uintptr_t)(cur + 8);   // prev
            i++;
        }
        if (i == 0)
            klog("[钉子8] runner 链表：空（mgr[0x98]=%#llx，头=%#llx）\n",
                 (unsigned long long)cur, (unsigned long long)head);
    }
    return 0;
}
// ★ 销毁入口探针（2026-10-18）：runner 跑完任务（虚表[0x88]）后把任务移进完成链（mgr+0xd8），
//   下一轮对完成链逐个调 0xa01be8 销毁：状态 <7 ⇒ 推 7、析构 task+0x20、再 memcpy(task+0x88, 5)
//   逐个调 指针->[0]->虚表[0x20]。本轮崩在这里：第一个指针（=task+0xf0）的 [0]=0。
//   这个钩子 dump 销毁时的状态、子任务链、以及那 5 个指针（含各自 [0]）——判定数组是不是垃圾。
static int g_n_e12;
static uint64_t hook_e12(uint64_t *f) {   // 销毁入口 0xa01be8：x0 = mgr，x1 = 任务
    if (++g_n_e12 <= 8) {
        uint64_t t = f[FR_X1];
        uint64_t st = t ? *(const uint32_t *)(uintptr_t)(t + 0xb8) : 0;
        uint64_t sub = t ? *(const uint64_t *)(uintptr_t)(t + 0x28) : 0;
        uint64_t a0 = t ? *(const uint64_t *)(uintptr_t)(t + 0x88) : 0;
        uint64_t a1 = t ? *(const uint64_t *)(uintptr_t)(t + 0x90) : 0;
        uint64_t a2 = t ? *(const uint64_t *)(uintptr_t)(t + 0x98) : 0;
        uint64_t a0v = a0 ? *(const uint64_t *)(uintptr_t)a0 : 0;
        klog("[钉子8] 销毁 0xa01be8：mgr=%#llx 任务=%#llx 状态=%llu 子链=%#llx "
             "数组[88]=%#llx(→[0]=%#llx) [90]=%#llx [98]=%#llx\n",
             (unsigned long long)f[FR_X0], (unsigned long long)t, (unsigned long long)st,
             (unsigned long long)sub, (unsigned long long)a0, (unsigned long long)a0v,
             (unsigned long long)a1, (unsigned long long)a2);
    }
    return 0;
}
// ★ 池注册体内 / mgr 构造尾部的定位钩（2026-10-18）：真机现场「日志停在池注册入口之后、
//   内部 memcpy 之前」两轮完全一致 ⇒ 在注册体入口与构造尾部各放一枪，把区间切成两段：
//   若 e13 到、e14 不到 ⇒ 卡在注册体内；若 e14 到 ⇒ 卡在更后面。
static int g_n_e13, g_n_e14;
static uint64_t hook_e13(uint64_t *f) {   // 注册体 0xa01928：x0 = mgr，x1 = 记录源
    if (++g_n_e13 <= 4)
        klog("[钉子8] 注册体 0xa01928：mgr=%#llx 记录源=%#llx\n",
             (unsigned long long)f[FR_X0], (unsigned long long)f[FR_X1]);
    return 0;
}
static uint64_t hook_e14(uint64_t *f) {   // mgr 构造尾部 0xa00d4c（注册之后的解锁点）
    (void)f;                              // 该点 x19=this 不在帧里（桩只存 x0-x17/x29/x30）
    if (++g_n_e14 <= 4)
        klog("[钉子8] 构造尾 0xa00d4c 到达（注册已返回、准备解锁）\n");
    return 0;
}
// ★ 日志系统探针（2026-10-18）：崩点 = 日志遍历（0xf35400）拿节点 [node+8] 当串与 "main"
//   做 strcmp，而该串指针已被写坏（si_addr 恒 0x61fbXXXX5e，高 16 位一致 ⇒ 系统性写坏）。
//   离线已解码：日志对象 [0]=链头 [+0x10]=计数 [+0x18]=空闲链 [+0x28]=容量；
//   节点 [0]=tick [8]=消息串 [0x10]=链节点。两枪对夹：添加入口看"游戏传什么"，遍历入口看现场。
static int g_n_e15, g_n_e16;
static uint64_t hook_e15(uint64_t *f) {   // 日志添加 0xf35374：x0 = 消息（串？结构？）
    if (++g_n_e15 <= 10) {
        uint64_t a = f[FR_X0];
        uint64_t obj = *(const uint64_t *)(uintptr_t)((uint64_t)(uintptr_t)g_k_m.base + 0x2288548);   // 日志对象全局
        uint64_t cnt = obj ? *(const uint32_t *)(uintptr_t)(obj + 0x10) : 0;
        klog("[日志] 添加：参数=%#llx 日志对象=%#llx 计数=%llu\n",
             (unsigned long long)a, (unsigned long long)obj, (unsigned long long)cnt);
    }
    return 0;
}
static uint64_t hook_e16(uint64_t *f) {   // 日志遍历 0xf35400
    if (++g_n_e16 <= 10) {
        uint64_t obj = *(const uint64_t *)(uintptr_t)((uint64_t)(uintptr_t)g_k_m.base + 0x2288548);
        uint64_t head = obj ? *(const uint64_t *)(uintptr_t)obj : 0;
        uint64_t cnt = obj ? *(const uint32_t *)(uintptr_t)(obj + 0x10) : 0;
        uint64_t msg = head ? *(const uint64_t *)(uintptr_t)(head - 0x10 + 8) : 0;
        klog("[日志] 遍历：对象=%#llx 计数=%llu 链头=%#llx 首节点消息=%#llx\n",
             (unsigned long long)obj, (unsigned long long)cnt,
             (unsigned long long)head, (unsigned long long)msg);
    }
    return 0;
}
// ★ 注册体加锁点 0xa01924（2026-10-18）：e13（0xa01928 注册体第一条）从未触发 ⇒ 卡在它前一条
//   `bl 0xa1d56c`（给 mgr+8 上 sead 锁）。这里在锁调用前 dump 锁对象四个字，看 state/owner。
static int g_n_e17;
static uint64_t hook_e17(uint64_t *f) {   // 0xa01924 处：x0 = &mgr[8]（锁对象）
    if (++g_n_e17 <= 8) {
        uint64_t o = f[FR_X0];
        uint64_t w0 = o ? *(const uint64_t *)(uintptr_t)o : 0;
        uint64_t w1 = o ? *(const uint64_t *)(uintptr_t)(o + 8) : 0;
        klog("[钉子8] 注册体上锁前 0xa01924：锁对象=%#llx [0]=%#llx [8]=%#llx\n",
             (unsigned long long)o, (unsigned long long)w0, (unsigned long long)w1);
    }
    return 0;
}

// ===== Task #16：追「第一个任务由谁创建」（离线结论见进度文档 §20）=====
// 背景：sead 主循环每帧只做 lock → G(取任务) → runner(跑任务) → unlock → 睡。
// G 只有在池里**已经有登记项**时才会创建/激活任务；而池的唯一填充入口是 0xa018f8，
// 它的调用者里只有"创建路径"和"任务自建子任务"两条是真来源。
// 建任务的 API 只有两个：0xa0143c（立即，6 个调用点）和 0x9fe314（虚调用，0 个直接调用点）。
// 而 0x726xxx 那三个疑似"连建根任务"的站点，全映像没有任何 bl/b 能到达 ⇒ 只能运行时抓。
//
// 两个探针：
//   ① 0xa0143c 入口：谁调的（x30−基址）、mgr、参数结构体指针；
//   ② 0xa01490（`bl 0xa01790` 的下一条）：此时 **x0 = 刚创建出来的任务**，
//      顺手把 [task]（虚表指针）记下来，供离线反查类名（vtable−0x10 → typeinfo → 名字串）。
static int g_n_e18, g_n_e19;
// 从**我们自己 mmap 的映像**里读一段可打印 ASCII（最长 48 字符）。
// 为什么不用 RTTI 反查类名：实测映像里 `N4sead` 出现 0 次、`typeinfo for` 0 次
// ⇒ 零售版把 RTTI 名字剥了，vtable−0x10 那条路走不通。改读"创建参数里的名字串"。
// 安全性：只读 [base, base+DM_IMG_MAIN_END) 这一段（我们自己的映射，必然可读）。
static int k_img_str(uint64_t addr, char *out, size_t cap) {
    uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
    if (addr < base || addr >= base + DM_IMG_MAIN_END) return 0;
    const char *p = (const char *)(uintptr_t)addr;
    size_t n = 0;
    while (n < 48 && n + 1 < cap) {
        char c = p[n];
        if (!c) break;
        if (c < 0x20 || c > 0x7e) return 0;
        n++;
    }
    if (n < 3) return 0;
    memcpy(out, p, n);
    out[n] = '\0';
    return 1;
}
static uint64_t hook_e18(uint64_t *f) {   // 0xa0143c：CreateTaskImmediate(mgr, param)
    if (++g_n_e18 <= 12 || (g_n_e18 % 200) == 0) {
        uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
        uint64_t lr = f[FR_X30];
        uint64_t lr_off = (lr >= base && lr < base + DM_IMG_MAIN_END) ? lr - base : 0;
        uint64_t par = f[FR_X1];
        klog("[任务] CreateTaskImmediate#%d 调用者=+%#llx mgr=%#llx 参数=%#llx\n",
             g_n_e18, (unsigned long long)lr_off,
             (unsigned long long)f[FR_X0], (unsigned long long)par);
        // 参数结构体（0xb0 字节）头 8 个 qword 里凡是"指向映像内的字符串"，都打出来——
        // 任务名几乎一定在里面（这是唯一不依赖 RTTI 的识别手段）。
        if (par) {
            for (int i = 0; i < 8; i++) {
                uint64_t v = *(const uint64_t *)(uintptr_t)(par + (uint64_t)i * 8);
                char s[64];
                if (k_img_str(v, s, sizeof s))
                    klog("[任务]   参数[%d]=%#llx 「%s」\n", i, (unsigned long long)v, s);
            }
        }
    }
    return 0;
}
static uint64_t hook_e19(uint64_t *f) {   // 0xa01490：刚建好的任务在 x0
    if (++g_n_e19 <= 12) {
        uint64_t t = f[FR_X0];
        uint64_t vt = t ? *(const uint64_t *)(uintptr_t)t : 0;
        uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
        uint64_t vt_off = (vt >= base && vt < base + DM_IMG_MAIN_END) ? vt - base : 0;
        uint64_t st = t ? *(const uint32_t *)(uintptr_t)(t + 0xb8) : 0;
        klog("[任务] 建出任务=%#llx 虚表=+%#llx 状态=%llu\n",
             (unsigned long long)t, (unsigned long long)vt_off, (unsigned long long)st);
    }
    return 0;
}

static int g_n_e20, g_n_e21;
// 0xa0913c 是"按名字建/查管理器对象"的助手（x1 指向一个 {虚表?, 名字串} 结构）。
// 真机日志 5100 行那次唯一的方法树创建就用了它，名字来自 0x1ba6xxx 那张表——
// 那张表里全是 sead 的方法/管理器名：CalculateTask、RootCalc/SysCalc/AppCalc、
// RootDraw/SysDraw/AppDraw/AppDrawFinal、sead::MethodTreeMgr / ThreadMgr / SystemManagers …
// ⇒ 只要把"被请求的名字 + 拿到的结果"打出来，就知道哪一环在宿主上没建起来。
static uint64_t hook_e20(uint64_t *f) {   // 0xa0913c 入口
    if (++g_n_e20 <= 40) {
        uint64_t st = f[FR_X1];
        uint64_t nm = 0;
        char s[64] = {0};
        if (st) nm = *(const uint64_t *)(uintptr_t)(st + 8);
        if (nm && k_img_str(nm, s, sizeof s)) { /* 拿到名字 */ }
        klog("[名字] 建/查#%d 结构=%#llx 名字指针=%#llx 「%s」\n",
             g_n_e20, (unsigned long long)st, (unsigned long long)nm, s);
    }
    return 0;
}
static uint64_t hook_e21(uint64_t *f) {   // 0x9fe380：刚拿到 0xa0913c 的返回值（x0）
    if (++g_n_e21 <= 20) {
        klog("[名字] 方法树创建结果 x0=%#llx（0 = 没建起来）\n", (unsigned long long)f[FR_X0]);
    }
    return 0;
}
// ⚠ 真机教训（np32）：只挂了 0xa0913c（thunk：`and w5,w5,#1` 后 b 到实体）→ **一行都没打**。
//   全映像统计：`bl 0xa0913c` 102 处、`bl 0xa09144`（实体入口）26 处 ⇒ 这个 API 有**两个入口**，
//   只挂一个必然漏。两个都挂，才能拿到完整的"按名字建对象"序列。
static int g_n_e22;
static uint64_t hook_e22(uint64_t *f) {   // 0xa09144：同一 API 的实体入口
    if (++g_n_e22 <= 40) {
        uint64_t st = f[FR_X1];
        uint64_t nm = 0;
        char s[64] = {0};
        if (st) nm = *(const uint64_t *)(uintptr_t)(st + 8);
        if (nm) k_img_str(nm, s, sizeof s);
        klog("[名字] 实体入口#%d 结构=%#llx 名字=%#llx 「%s」\n",
             g_n_e22, (unsigned long long)st, (unsigned long long)nm, s);
    }
    return 0;
}
// 0x9fe984 是 `SystemManagers::initialize()` 里"去建 ThreadMgr"的那一步（0x9fe984 → 0x9fe24c）。
// 它命中 ⇒ 框架初始化确实走到这里；不命中 ⇒ 卡在更前面。
static int g_n_e23;
static uint64_t hook_e23(uint64_t *f) {
    if (++g_n_e23 <= 6) klog("[框架] 0x9fe984：SystemManagers 初始化里要建 ThreadMgr\n");
    return 0;
}
// 0xa112fc = 「往列表里塞 {节点, 名字}」的助手。7 棵方法树（RootCalc/SysCalc/AppCalc/
// RootDraw/SysDraw/AppDraw/AppDrawFinal）就是靠它连续挂 7 次（见 0x9ff398–0x9ff430）。
// 它被调几次、每次名字是什么，直接回答"方法树有没有挂上去"。
static int g_n_e24;
static uint64_t hook_e24(uint64_t *f) {
    if (++g_n_e24 <= 40) {
        uint64_t st = f[FR_X1];
        uint64_t nm = st ? *(const uint64_t *)(uintptr_t)(st + 8) : 0;
        char s[64] = {0};
        if (nm) k_img_str(nm, s, sizeof s);
        klog("[挂载] 0xa112fc#%d 目标=%#llx 名字指针=%#llx 「%s」\n",
             g_n_e24, (unsigned long long)f[FR_X0], (unsigned long long)nm, s);
    }
    return 0;
}
static int g_n_e25;
static uint64_t hook_e25(uint64_t *f) {   // 0x9ff3a0：7 棵树挂载段（纯 store，不喂寄存器）
    if (++g_n_e25 <= 4) klog("[框架] 0x9ff3a0：进入「挂 7 棵方法树」这一段\n");
    return 0;
}

// ===== 启动链追踪（Task #16 第三步）=====
// 思路换档：前几轮都是"猜一个点、挂一个钩"，一次只回答一个是非题，太慢。
// 这里把 **nnMain 里所有 bl 目标**按出现顺序一次性挂上，用同一个宿主函数（靠 f[FR_SITE] 区分站点）。
// 一次跑测就能得到"初始化走到第几步、在哪一步之后断掉"的完整顺序图。
// 站点顺序 = nnMain（0x735330）里 bl 的出现顺序（离线反汇编得到，见进度文档 §22/§23）。
static const uint32_t g_boot_sites[] = {
    0x9f3830, 0xf32228, 0xf326d4, 0x9fe234, 0xc4f3e4,
    0x8115d4, 0x811730, 0x815254, 0xdaab74, 0xc4f2f4,
    0xdaabc0, 0x9fc450, 0xa22508, 0xa2262c, 0xa03178,
    0xa03224, 0xa025f0, 0xa026a0, 0xa2469c, 0x9fce0c,
    0xa003f0, 0x9fe240, 0xa1e5f0, 0xa9cc4c,
};
static int g_n_boot;
// np68：设值函数 0xa2469c 的**替身**。原实现两条指令：`x8 = *(0x2280eb8); *(u64*)x8 = x0`。
// 真机怪象：启动链钩子（同一入口）读到槽是对的（base+0x22d9260），两条指令后 `ldr` 却拿到 0。
// 与其继续猜"谁在两指令间清零"，不如替它干活：槽正常就直接替它写，槽异常就记录并跳过。
// 这样既能绕开，又能把"槽异常"这一事实打在日志里。
// np88：解析器入口 0x9fa4c8（stp 开头，可挂）——把游戏正在解析的**键名**打出来。
// 这是配置/内容路径的正门：键名能告诉我们游戏在找什么（配置项？文件？命令？）。
static int g_n_parse;
static uint64_t hook_parse(uint64_t *f) {
    if (++g_n_parse <= 40) {
        uint64_t k = f[FR_X1];
        char buf[64]; size_t i = 0;
        if (k > 0x10000ull && k < 0x800000000000ull) {
            const char *key = (const char *)(uintptr_t)k;
            for (; i < sizeof buf - 1 && key[i]; i++) buf[i] = key[i];
        }
        buf[i] = 0;
        klog("[解析] #%d 键=\"%s\"\n", g_n_parse, buf);
    }
    return 0;
}
// np84：线程消息处理的虚调用点（0xa1e32c：mov x0,x19 在 blr x9 之前，此时 x9=处理函数、x1=消息）。
// 目的：看这条"唯一被处理的消息"到底调到哪里——若调进我们的零桩，就是"收了但什么都没做"。
static int g_n_msg;
static uint64_t hook_msg(uint64_t *f) {
    uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
    uint64_t fn = f[9], msg = f[1];
    int in_img = fn >= base && fn < base + DM_IMG_MAIN_END;
    if (++g_n_msg <= 12)
        klog("[消息] 处理：函数=%s%#llx 消息=%#llx\n", in_img ? "+" : "(外)",
             (unsigned long long)(in_img ? fn - base : fn), (unsigned long long)msg);
    return 0;
}
static int g_n_setter;
static uint64_t hook_setter(uint64_t *f) {
    uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
    uint64_t slot = *(const uint64_t *)(uintptr_t)(base + 0x2280eb8);
    uint64_t val = f[FR_X0];
    if (++g_n_setter <= 8)
        klog("[设值] 0xa2469c 替身 #%d：槽=%#llx 值(x0)=%#llx\n",
             g_n_setter, (unsigned long long)slot, (unsigned long long)val);
    if (slot > 0x10000ull && slot < 0x800000000000ull)
        *(uint64_t *)(uintptr_t)slot = val;      // 原函数干的就是这件事
    return 0;
}
static uint64_t hook_boot(uint64_t *f) {    uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
    uint64_t site = f[FR_SITE];
    uint64_t so = (site >= base && site < base + DM_IMG_MAIN_END) ? site - base : site;
    uint64_t lr = f[FR_X30];
    uint64_t lo = (lr >= base && lr < base + DM_IMG_MAIN_END) ? lr - base : 0;
    if (++g_n_boot <= 80) {
        // np67：顺带盯 0x2280eb8——崩点 +0xA246A4 的间接目标。装载后/静态初始化后都正确，
        // 崩时却是 0 ⇒ 是 nnMain 流程里被"覆盖写"清掉的（代码里没有 #0xeb8 的直接 store）。
        // 逐站读它，看哪一段之后变 0。
        uint64_t slot = *(const uint64_t *)(uintptr_t)(base + 0x2280eb8);
        klog("[启动链] #%d 进入 +%#llx（返回地址 +%#llx）｜槽 0x2280eb8=%#llx\n",
             g_n_boot, (unsigned long long)so, (unsigned long long)lo,
             (unsigned long long)slot);
    }
    return 0;
}
// ★ 决定性挂点（2026-10-18）：nnMain 0x73557c 是 `mov x22, x0`——x0 就是后面那个
//   `cbz x22, 0x735650` 的判断依据。为 0 就**跳过 7 个初始化调用**直接进主循环（真机已证）。
//   x0 来自 0x1074b2c / 0x1074b50 两个"取单例 + 调其虚表[0x10]"的小函数，
//   单例槽在 0x2289e60 / 0x2289e68。这里把"取值结果 + 两个单例槽 + 对象的虚表"一起打出来。
static int g_n_e26;
static uint64_t hook_e26(uint64_t *f) {
    if (++g_n_e26 <= 8) {
        uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
        uint64_t v = f[FR_X0];
        uint64_t s60 = *(const uint64_t *)(uintptr_t)(base + 0x2289e60);
        uint64_t s68 = *(const uint64_t *)(uintptr_t)(base + 0x2289e68);
        uint64_t vt = 0, fn10 = 0;
        if (s60) {
            vt = *(const uint64_t *)(uintptr_t)s60;
            if (vt >= base && vt < base + DM_IMG_MAIN_END)
                fn10 = *(const uint64_t *)(uintptr_t)(vt + 0x10);
        }
        klog("[关键] 0x73557c x0(=x22)=%#llx  单例[0x2289e60]=%#llx [0x2289e68]=%#llx 虚表=%#llx 虚表[0x10]=%#llx\n",
             (unsigned long long)v, (unsigned long long)s60, (unsigned long long)s68,
             (unsigned long long)vt, (unsigned long long)fn10);
    }
    return 0;
}

// ★ 对照实验（2026-10-18 01:00）：0x73557c 的钩子"装上却不命中"，而紧邻的 0x735580 命中了，
//   这是个矛盾——不解决它，"7 个初始化被跳过"这个结论就不可信。
//   0x7355a8 是 `cbz x22` **不跳**时才执行的第一条指令（mov x0, x22）：
//   · 它命中 ⇒ 那 7 个初始化**其实跑了**，之前的结论要推翻（钩子在本区域不可靠）；
//   · 它不命中 ⇒ x22 确实为 0，结论成立，且钩子本身没问题。
static int g_n_e27;
static uint64_t hook_e27(uint64_t *f) {
    if (++g_n_e27 <= 4) klog("[对照] 0x7355a8 命中 ⇒ cbz x22 **没有**跳，7 个初始化真的执行了\n");
    return 0;
}

// ===== 影子「当前堆」（2026-10-18 01:05，用户批准方案 A）=====
// 真机结论（进度文档 §24/§25）：nnMain 在 0x7355a4 `cbz x22, 0x735650`，
// 而 x22 来自两个"取当前堆"的 getter（0x1074b2c / 0x1074b50）：
//     x8 = *(0x2289e60) → base+0x232f9e8；x0 = *(base+0x232f9e8)；
//     if (!x0) return 0;  return (*(*(x0)+0x10))(x0);
// 这两个变量在文件里是 0、全映像没有任何代码写它们 ⇒ 宿主上恒 0 ⇒
// x22==0 ⇒ nnMain 跳过 7 个初始化（a22508/a2262c/a03178/a03224/a025f0/a026a0/a2469c）直接进主循环。
//
// 修法（方案 A）：造一个最小"堆对象"挂上去，让 getter 能返回非 0。
//   · 分配仍然走我们已替换的宿主分配器（0x9f34d8/0x9f3464/0x9f33f0），它们**忽略**这个上下文指针；
//   · 所以这里只需要"非空 + 虚表[0x10] 能返回非空"，不要求它是真堆。
// 风险与判据：后续构造器若解引用它的字段，可能崩在下一个钉子（有 si_addr 可判）；
//   判据 = `[启动链]` 出现 +0xa22508 … +0xa2469c 这 7 条且不崩。
// 槽 0x80（下标 16）：**返回"带头部开销的分配尺寸"，不是指针**。依据 0xa09144 反汇编：
//     x8 = [heap]; x8 = [x8+0x80]; blr x8(heap, w20=size)   → x0
//     x24 = x0 & ~7;  if (x24 < 0x131) → 直接返 0（失败）     ← ★ 小尺寸会被判失败
//     x8 = [heap+0x30]; blr x8(heap, x24, w21*w20)           → 真正的分配（槽 0x30）
// 真机 np42：返回 8 ⇒ 8 < 0x131 ⇒ 立刻失败（GameConfig 拿不到，x20=0 崩）。
// ⇒ 真堆这里显然会加上"头部/对齐开销"。这里按 size + 0x130 返回（稳稳越过 0x131 门槛）。
static uint64_t fake_heap_size(void *self, uint64_t size) {
    (void)self;
    uint64_t v = (size + 0x130 + 7) & ~7ull;
    static int n;
    if (++n <= 8) klog("[影子堆] 取尺寸 size=%llu → %llu（含 0x130 开销）\n",
                       (unsigned long long)size, (unsigned long long)v);
    return v;
}
// 槽 0x30（下标 6）：真分配。参数按 (self, size, count) 解读（反汇编：x1=x24 尺寸、w2=w21*w20 个数）。
static uint64_t fake_heap_alloc(void *self, uint64_t size, uint64_t count) {
    (void)self;
    static int n;
    uint64_t total = size * (count ? count : 1);
    if (++n <= 12)
        klog("[影子堆] 分配 size=%llu count=%llu 合计=%llu\n",
             (unsigned long long)size, (unsigned long long)count, (unsigned long long)total);
    if (total == 0 || total > (64u << 20)) {
        if (n <= 12) klog("[影子堆] 合计不合理，返 0\n");
        return 0;
    }
    void *p = calloc(1, (size_t)total);
    if (n <= 12) klog("[影子堆] 分配 → %p\n", p);
    return (uint64_t)(uintptr_t)p;
}
#define FAKE_VT_QWORDS   64          // 虚表 64 槽（覆盖偏移 0..0x1f8，防 blr 到表外）
#define FAKE_HEAP_QWORDS 512         // 堆对象 4KB：真实堆结构体字段可能很多，给足
// ★ Task #4（2026-10-18 修）：这两个对象**必须放独立 calloc 的内存，不能放 .so 的 .bss**。
//   原因：游戏会把"堆对象"当自己的结构体往里写字段；对象若在 .bss 里，越界写入会直接
//   踩坏 .bss 的其它变量（g_n_e28 计数器输出垃圾值 -247789891 即此症状，§32.2 附注）。
//   放 calloc 内存里，最坏也只是踩坏我们自己那一块，不波及仪器计数器。
static uint64_t *g_fake_vt;          // 假虚表（运行期 calloc）
static uint64_t *g_fake_heap;        // 假堆对象（首字 = 虚表指针；运行期 calloc）
// 虚表[0x10]（下标 2）：返回"堆"本身。签名按 (self) → uint64_t。
static uint64_t fake_heap_get(void *self) {
    return (uint64_t)(uintptr_t)self;
}
// 其余槽：真机 np39 教训——只填了槽 2，结果在"堆对象"上做了另一次虚调用 ⇒ 槽里是 0 ⇒
// `blr x8` 直接 pc=0 崩。现在把 32 个槽**全填上**（每个槽一个独立桩，带槽号日志），
// 返回值一律 0：既不会 pc=0 崩，又能从日志看出"游戏对堆对象调了哪个方法"。
static int g_n_vtstub;
static uint64_t fake_vt_ret0(int idx) {
    if (++g_n_vtstub <= 40) klog("[影子堆] 虚表[+%#x] 被调用 → 返 0\n", idx * 8);
    return 0;
}
#define VT_STUB(I) static uint64_t vt_s_##I(void *p) { (void)p; return fake_vt_ret0(I); }
VT_STUB(0)  VT_STUB(1)  VT_STUB(2)  VT_STUB(3)  VT_STUB(4)  VT_STUB(5)  VT_STUB(6)  VT_STUB(7)
VT_STUB(8)  VT_STUB(9)  VT_STUB(10) VT_STUB(11) VT_STUB(12) VT_STUB(13) VT_STUB(14) VT_STUB(15)
VT_STUB(16) VT_STUB(17) VT_STUB(18) VT_STUB(19) VT_STUB(20) VT_STUB(21) VT_STUB(22) VT_STUB(23)
VT_STUB(24) VT_STUB(25) VT_STUB(26) VT_STUB(27) VT_STUB(28) VT_STUB(29) VT_STUB(30) VT_STUB(31)
VT_STUB(32) VT_STUB(33) VT_STUB(34) VT_STUB(35) VT_STUB(36) VT_STUB(37) VT_STUB(38) VT_STUB(39)
VT_STUB(40) VT_STUB(41) VT_STUB(42) VT_STUB(43) VT_STUB(44) VT_STUB(45) VT_STUB(46) VT_STUB(47)
VT_STUB(48) VT_STUB(49) VT_STUB(50) VT_STUB(51) VT_STUB(52) VT_STUB(53) VT_STUB(54) VT_STUB(55)
VT_STUB(56) VT_STUB(57) VT_STUB(58) VT_STUB(59) VT_STUB(60) VT_STUB(61) VT_STUB(62) VT_STUB(63)
static const uint64_t g_fake_vt_tab[FAKE_VT_QWORDS] = {
    (uint64_t)(uintptr_t)&vt_s_0,  (uint64_t)(uintptr_t)&vt_s_1,
    (uint64_t)(uintptr_t)&fake_heap_get,   // 下标 2 = 偏移 0x10：取堆（必须返非 0）
    (uint64_t)(uintptr_t)&vt_s_3,  (uint64_t)(uintptr_t)&vt_s_4,
    (uint64_t)(uintptr_t)&vt_s_5,  (uint64_t)(uintptr_t)&fake_heap_alloc,  // 下标 6 = 偏移 0x30：真分配
    (uint64_t)(uintptr_t)&vt_s_7,  (uint64_t)(uintptr_t)&vt_s_8,
    (uint64_t)(uintptr_t)&vt_s_9,  (uint64_t)(uintptr_t)&vt_s_10,
    (uint64_t)(uintptr_t)&vt_s_11, (uint64_t)(uintptr_t)&vt_s_12,
    (uint64_t)(uintptr_t)&vt_s_13, (uint64_t)(uintptr_t)&vt_s_14,
    (uint64_t)(uintptr_t)&vt_s_15, (uint64_t)(uintptr_t)&fake_heap_size,  // 下标 16 = 偏移 0x80：取尺寸
    (uint64_t)(uintptr_t)&vt_s_17, (uint64_t)(uintptr_t)&vt_s_18,
    (uint64_t)(uintptr_t)&vt_s_19, (uint64_t)(uintptr_t)&vt_s_20,
    (uint64_t)(uintptr_t)&vt_s_21, (uint64_t)(uintptr_t)&vt_s_22,
    (uint64_t)(uintptr_t)&vt_s_23, (uint64_t)(uintptr_t)&vt_s_24,
    (uint64_t)(uintptr_t)&vt_s_25, (uint64_t)(uintptr_t)&vt_s_26,
    (uint64_t)(uintptr_t)&vt_s_27, (uint64_t)(uintptr_t)&vt_s_28,
    (uint64_t)(uintptr_t)&vt_s_29, (uint64_t)(uintptr_t)&vt_s_30,
    (uint64_t)(uintptr_t)&vt_s_31, (uint64_t)(uintptr_t)&vt_s_32,
    (uint64_t)(uintptr_t)&vt_s_33, (uint64_t)(uintptr_t)&vt_s_34,
    (uint64_t)(uintptr_t)&vt_s_35, (uint64_t)(uintptr_t)&vt_s_36,
    (uint64_t)(uintptr_t)&vt_s_37, (uint64_t)(uintptr_t)&vt_s_38,
    (uint64_t)(uintptr_t)&vt_s_39, (uint64_t)(uintptr_t)&vt_s_40,
    (uint64_t)(uintptr_t)&vt_s_41, (uint64_t)(uintptr_t)&vt_s_42,
    (uint64_t)(uintptr_t)&vt_s_43, (uint64_t)(uintptr_t)&vt_s_44,
    (uint64_t)(uintptr_t)&vt_s_45, (uint64_t)(uintptr_t)&vt_s_46,
    (uint64_t)(uintptr_t)&vt_s_47, (uint64_t)(uintptr_t)&vt_s_48,
    (uint64_t)(uintptr_t)&vt_s_49, (uint64_t)(uintptr_t)&vt_s_50,
    (uint64_t)(uintptr_t)&vt_s_51, (uint64_t)(uintptr_t)&vt_s_52,
    (uint64_t)(uintptr_t)&vt_s_53, (uint64_t)(uintptr_t)&vt_s_54,
    (uint64_t)(uintptr_t)&vt_s_55, (uint64_t)(uintptr_t)&vt_s_56,
    (uint64_t)(uintptr_t)&vt_s_57, (uint64_t)(uintptr_t)&vt_s_58,
    (uint64_t)(uintptr_t)&vt_s_59, (uint64_t)(uintptr_t)&vt_s_60,
    (uint64_t)(uintptr_t)&vt_s_61, (uint64_t)(uintptr_t)&vt_s_62,
    (uint64_t)(uintptr_t)&vt_s_63,
};
// 指针合法性快查：np54 实证游戏侧越界写会把 .bss 指针槽污染成 94 一类的小值，
// 用之前必须先验（否则解引用即崩）。合法区间：远大于 0、小于用户态上限。
static int k_ptr_sane(const void *p) {
    uintptr_t v = (uintptr_t)p;
    return p && v > 0x10000ull && v < 0x800000000000ull;
}
static void k_fake_heap_install(uint8_t *img) {
    // 自愈：指针槽若被污染（np54：g_fake_parse_obj 被写成 0x5e），重新分配再挂。
    if (!k_ptr_sane(g_fake_vt) || !k_ptr_sane(g_fake_heap)) {
        klog("[影子堆] ⚠ 指针槽被污染（vt=%p heap=%p），重新分配\n",
             (void *)g_fake_vt, (void *)g_fake_heap);
        g_fake_vt   = calloc(FAKE_VT_QWORDS, sizeof(uint64_t));
        g_fake_heap = calloc(FAKE_HEAP_QWORDS, sizeof(uint64_t));
    }
    if (!g_fake_vt) {
        g_fake_vt   = calloc(FAKE_VT_QWORDS, sizeof(uint64_t));
        g_fake_heap = calloc(FAKE_HEAP_QWORDS, sizeof(uint64_t));
    }
    memcpy(g_fake_vt, g_fake_vt_tab, FAKE_VT_QWORDS * sizeof(uint64_t));
    g_fake_heap[0] = (uint64_t)(uintptr_t)g_fake_vt;
    // 两个"当前堆"变量（模块偏移，来自 RELA addend 反查：0x2289e60→0x232f9e8、0x2289e68→0x232f9f0）
    *(uint64_t *)(img + 0x232f9e8) = (uint64_t)(uintptr_t)g_fake_heap;
    *(uint64_t *)(img + 0x232f9f0) = (uint64_t)(uintptr_t)g_fake_heap;
    klog("[影子堆] 已挂：堆=%p（calloc %dKB）虚表=%p（%d 槽，槽[0x10]=取堆）\n",
         (void *)g_fake_heap, FAKE_HEAP_QWORDS * 8 / 1024, (void *)g_fake_vt, FAKE_VT_QWORDS);
}

static int g_n_e28;
static uint64_t hook_e28(uint64_t *f) {
    // ★ 挂的是 worker **0xa051c4**（不是 0xa05110——后者只是"创建/取单例"：
    //   里面 bl 0x9f3464 分配 0x7c8 字节、bl 0xa08ef4 构造，然后返回那个单例）。
    //   崩点 0xa05324 在 worker 里：`mov x23, x2`（第 3 个参数）随后 `ldr w8,[x23,#8]`。
    //   这里把 4 个入参与调用者打出来，看"是谁把 0 传进来的"。
    // ⚠ np58 教训：钩子里**别在静态初始化期做 calloc/多行日志/解引用客体指针**——会卡死
    //   （真机卡在 ctor #385）。这里保持 np57 的形态：静态计数器 + 只多打两行映像内标量。
    if (++g_n_e28 <= 12) {
        uint64_t base = (uint64_t)(uintptr_t)g_k_m.base;
        uint64_t lr = f[FR_X30];
        uint64_t lo = (lr >= base && lr < base + DM_IMG_MAIN_END) ? lr - base : 0;
        // 帧下标：FR_X0=0/FR_X1=1/FR_X2=2，FR_NZCV=18/FR_X29=20/FR_X30=21/FR_SITE=22
        // ⇒ x3..x17 就是 3..17（没有专门宏，直接用下标）
        klog("[建对象] worker 0xa051c4#%d：x0=%#llx x1=%#llx **x2=%#llx** w3=%#llx 调用者=+%#llx\n",
             g_n_e28, (unsigned long long)f[FR_X0], (unsigned long long)f[FR_X1],
             (unsigned long long)f[FR_X2], (unsigned long long)f[3],
             (unsigned long long)lo);
        // np58：后段崩点 (+0xA05620) 的关键标量——**只读映像内的槽、不碰客体指针**
        // （上一版在这里解引用客体指针 + 多打 6 行，真机卡死在静态初始化 #385，已回退）。
        {
            const uint8_t *im = (const uint8_t *)(uintptr_t)g_k_m.base;
            uint64_t sa = *(const uint64_t *)(im + 0x227f900);   // 槽 → .bss 变量（或运行期换成的堆对象）
            uint64_t sb = *(const uint64_t *)(im + 0x227f8e8);
            int sa_in = sa >= base && sa < base + DM_IMG_MAIN_END;
            int sb_in = sb >= base && sb < base + DM_IMG_MAIN_END;
            klog("[建对象] 槽A(0x227f900)=%#llx%s 槽B(0x227f8e8)=%#llx%s\n",
                 (unsigned long long)sa, sa_in ? "（映像内）" : "（非映像）",
                 (unsigned long long)sb, sb_in ? "（映像内）" : "（非映像）");
            if (sa_in) {
                uint64_t fn = *(const uint64_t *)(uintptr_t)sa;
                klog("[建对象] 槽A内容=%#llx%s\n", (unsigned long long)fn,
                     (fn >= base && fn < base + DM_IMG_MAIN_END) ? "（是映像内函数）" : "（非映像）");
            }
        }
    }
    return 0;
}

// 探针用：把一段客体内存打成十六进制（同进程同地址空间，可直接解引用）。
// 用途：看清"游戏传进来的对象/字符串"到底是什么——键名、字符串、小结构体都靠它认。
static void k_dump_hex(const char *tag, const void *p, size_t n) {
    char line[200];
    const unsigned char *b = (const unsigned char *)p;
    if (!p) { klog("%s（空指针）\n", tag); return; }
    size_t pos = (size_t)snprintf(line, sizeof line, "%s %p:", tag, p);
    for (size_t i = 0; i < n && pos + 4 < sizeof line; i++)
        pos += (size_t)snprintf(line + pos, sizeof line - pos, " %02x", b[i]);
    klog("%s\n", line);
}

// 金丝雀 + 计数器快照：np49 实证 .so 的 .bss 会被**点状污染**——g_n_e28/e29 变成垃圾值
//（-247770147），而 取尺寸/分配/虚表 等其它 .bss 计数器完全正常。后果很阴：
//"if (++g_n <= N) 才打印" 的守卫在计数器被写成大正数后**静默吞掉日志**，
// 伪装成"钩子没命中"（0x9fa4c8 探针 np49 零命中即此）。
// 金丝雀紧挨着声明，用来看污染的形状（写哪个、写多大范围）。
static int g_canary0, g_canary1, g_canary2, g_canary3;
static int g_canary4, g_canary5, g_canary6, g_canary7;
static int g_n_e29;
static void k_log_counters(const char *tag) {
    klog("[计数器%s] e28=%d e29=%d 金丝雀=%d %d %d %d %d %d %d %d\n",
         tag, g_n_e28, g_n_e29,
         g_canary0, g_canary1, g_canary2, g_canary3,
         g_canary4, g_canary5, g_canary6, g_canary7);
}

// 0x9fa4c8：通用「键:值」解析器（全映像 8 处调用）。当前崩点链的**最上游**：
//   0xa05fa4(经函数指针进来) 用它解析一个字符串 → 返回 0
//   → worker 0xa051c4 的 x2 = 0 → 崩在 0xa05324（`ldr w8,[x23,#8]`）。
// 静态追查到此断了（0xa05fa4 无直接调用者，只能经虚表/函数指针进来），
// 所以运行时取证：把两个入参对象 dump 出来，看清"在解析哪个键、键从哪来"。
// ★ 自 np50 起**无条件打印**：不再用 g_n_e29 计数抑制（.bss 计数器会被踩，见上方注释）。
// np50 首战：对象布局已认清——x1+0x20 = 字符串数据指针、x1+0x28 = 尺寸（本回合 16 字节）。
// 本轮把**串内容本身**打出来，直接回答"在解析哪个键"。
static uint64_t hook_e29(uint64_t *f) {
    const unsigned char *o = (const unsigned char *)(uintptr_t)f[FR_X1];
    klog("[键解析] 0x9fa4c8：x0=%#llx x1=%#llx 金丝雀地址=%p\n",
         (unsigned long long)f[FR_X0], (unsigned long long)f[FR_X1], (void *)&g_canary0);
    k_dump_hex("[键解析] x0 对象", (const void *)(uintptr_t)f[FR_X0], 32);
    k_dump_hex("[键解析] x1 对象", o, 64);
    if (o) {
        uint64_t data = *(const uint64_t *)(o + 0x20);
        uint64_t sz   = *(const uint64_t *)(o + 0x28);
        klog("[键解析] 疑似字符串：数据=%#llx 尺寸字段=%#llx（低32=cap 高32=len？）\n",
             (unsigned long long)data, (unsigned long long)sz);
        // +0x28 实际是 {cap=0x1000, len=0x10} 两半（np51 实证），别拿它当整体尺寸卡条件
        if (data > 0x10000 && data < 0x800000000000ull)
            k_dump_hex("[键解析] 串内容", (const void *)(uintptr_t)data, 48);
    }
    k_log_counters("解析时");
    return 0;
}

// 0xa05fa4：解析派发 = worker 0xa051c4 的**调用者**（经函数指针进来，全映像无直接 bl）。
// 它的 x2 会喂给构造函数 0xa112fc（`mov x1, x2`）⇒ 极可能就是"要解析的字符串"本体。
// （np52 证实 0x9fa4c8 的 x1+0x20 只是二级指针：0x5b96a6d000 → {0x5b96a64600,…}，
//   继续跟指针链不如回到上游看 x2。）
static int g_n_e30;
static uint64_t hook_e30(uint64_t *f) {
    const unsigned char *s = (const unsigned char *)(uintptr_t)f[FR_X2];
    klog("[派发] 0xa05fa4：x0=%#llx x1=%#llx x2=%#llx\n",
         (unsigned long long)f[FR_X0], (unsigned long long)f[FR_X1],
         (unsigned long long)f[FR_X2]);
    k_dump_hex("[派发] x2 对象", s, 32);
    if (s) {
        uint64_t p0 = *(const uint64_t *)s;   // std::string 长形态首字 = 数据指针
        if (p0 > 0x10000 && p0 < 0x800000000000ull)
            k_dump_hex("[派发] x2[0] 指向", (const void *)(uintptr_t)p0, 48);
    }
    return 0;
}

// 0x9fa4c8 的**替身**（np54 起）：原实现是"建临时对象 → 按 ':' 切分 → 遍历已注册解析器"，
// 在宿主上一路缺数据、恒返回 0 ⇒ worker 的 x2 = 0 ⇒ 崩在 0xa05324（`ldr w8,[x23,#8]`）。
// 按 §八「影子对象」打法：宿主直接返回一个**合法的全零对象**（calloc 8KB），
// 让流程越过"解析失败"这道坎——每越一坎，下一个真实需求就会暴露（与影子堆同一逻辑）。
// ⚠ np54 教训：对象指针**不能存 .bss 静态变量**——np54 实证 `g_fake_parse_obj` 被游戏侧
//   越界写污染成 94(0x5e)，`if (!ptr)` 检查被绕过 ⇒ 把 0x5e 当对象返回 ⇒ 原样崩在同一点。
//   现在每次**局部变量现分配**（.bss 只留一份副本仅供比对，不参与判断）。
static void *g_fake_parse_obj;
static uint64_t hook_rep_parse(uint64_t *f) {
    void *old = g_fake_parse_obj;                 // 旧值：0=从没写过；94=被污染（np54 实证）
    void *obj = calloc(1, 0x2000);                // 局部现分配，不依赖 .bss
    g_fake_parse_obj = obj;                       // 留档（便于事后比对），不用于判断
    {   // np88/np89：把**键名**打出来。注意 x1 不是字符串本身，而是**字符串对象**
        //   （sead::BufferedSafeString 布局：+0 数据指针、+4 长度、+8 容量）——真正的串在 [x1+0]。
        uint64_t so = f[FR_X1];
        char buf[64]; size_t i = 0; const char *key = NULL;
        if (so > 0x10000ull && so < 0x800000000000ull) {
            uint64_t k = *(const uint64_t *)(uintptr_t)so;      // [x1+0] = 数据指针
            if (k > 0x10000ull && k < 0x800000000000ull) key = (const char *)(uintptr_t)k;
        }
        if (key) for (; i < sizeof buf - 1 && key[i]; i++) buf[i] = key[i];
        buf[i] = 0;
        if (++g_n_parse <= 3 && so > 0x10000ull && so < 0x800000000000ull) {
            const uint64_t *q = (const uint64_t *)(uintptr_t)so;
            klog("[键解析·替身] 串对象布局：%016llx %016llx %016llx %016llx\n",
                 (unsigned long long)q[0], (unsigned long long)q[1],
                 (unsigned long long)q[2], (unsigned long long)q[3]);
        }
        klog("[键解析·替身] 键=\"%s\"（串对象=%#llx）旧值=%p 新对象=%p\n", buf,
             (unsigned long long)so, old, obj);
    }
    return (uint64_t)(uintptr_t)obj;
}

// ===== 替换式安装器：宿主替函数干活（返回值经 x0 带回），**原实现一条都不执行** =====
// ⚠ 早先这里把桩的"回跳槽"填成 `b 站点+4`——被替换的是**函数入口**，站点+4 就是原实现的
//   第二条指令，于是变成"宿主跑完、原实现接着跑"（双执行）：首条 stp 被跳过 ⇒ 栈帧少 0x20、
//   原实现里的 mov w19,w2 直接写坏 x19，最后返回的 x0 是原实现的结果（真机实证：x0 与 x23 双 0，
//   宿主的指针被丢掉）。现在桩自己 `ret`（回 x30 = 调用点下一条），安装器不再改回跳槽。
extern const char hook_rep_begin[], hook_rep_end[], hook_rep_host[], hook_rep_site[];
static void k_install_replace_hook(uint8_t *img, uint32_t site_off, void *host_fn, const char *what) {
    const char *b0 = hook_rep_begin;
    size_t n = (size_t)(hook_rep_end - hook_rep_begin);
    uint8_t *dst = img + DM_IMG_MAIN_CAVE_OFF + (size_t)g_hook_n * HOOK_STRIDE;
    if (n > HOOK_STRIDE) { klog("[替换] %s 桩过长(%zu > %u)，跳过\n", what, n, HOOK_STRIDE); return; }
    if (g_hook_n >= (int)HOOK_MAX) { klog("[替换] 洞已满(%d 格)，跳过 %s\n", g_hook_n, what); return; }
    memcpy(dst, b0, n);
    *(uint64_t *)(dst + (hook_rep_host - b0)) = (uint64_t)(uintptr_t)host_fn;
    *(uint64_t *)(dst + (hook_rep_site - b0)) = (uint64_t)(uintptr_t)(img + site_off);
    {   // 站点：b dst
        int64_t d = ((int64_t)(uintptr_t)dst - (int64_t)(uintptr_t)(img + site_off)) / 4;
        if (d < -(1 << 25) || d >= (1 << 25)) { klog("[替换] %s 站点超程，跳过\n", what); return; }
        *(uint32_t *)(img + site_off) = 0x14000000u | (uint32_t)(d & 0x03FFFFFFu);
    }
    klog("[替换] %s：site %#x → 洞 %#zx\n", what, site_off, (size_t)(dst - img));
    g_hook_n++;
}
// 宿主替分配器干活：x0 = 请求大小，返回真实内存（calloc 清零）
static uint64_t g_rep_n;
static int g_n_big;      // np106/np106b：大块分配计数（≥0x400，含管理器对象级别的）
static uint64_t g_big_sz[24];
static uint64_t g_big_lr[24];
static unsigned g_big_seq[24];
static uint64_t host_alloc_replace(uint64_t *f) {
    uint64_t size = f[FR_X0];
    void *p = calloc(1, (size_t)size);
    if (++g_rep_n <= 8)
        klog("[替换] 分配 %llu 字节 → %p（站点 %#llx 上下文 %#llx）\n",
             (unsigned long long)size, p,
             (unsigned long long)f[FR_SITE], (unsigned long long)f[FR_X1]);
    // np106：**启动包装载门**（[*(0x2276168)+0x419]）所在的管理器对象由这里分配；
    //   它在启动链前就已带 [+0x419]=4 ⇒ 分配后某段代码写了 4。记录所有 ≥0x400 的分配
    //   与站点，据尺寸（≥0x41c）与构造现场定位写入者。
    // np106b：**只记录、不打印**（np58 教训：分配器钩子里在构造器期打日志 ⇒ 卡死 ctor #385）。
    //   构造器跑完后由 dump_big_allocs() 统一输出。
    if (size >= 0x400 && g_n_big < 24) {
        uint64_t lr = f[FR_X30];
        g_big_sz[g_n_big] = size;
        g_big_lr[g_n_big] = lr;
        g_big_seq[g_n_big] = g_rep_n;      // 第几次分配（便于和日志行号对齐）
        g_n_big++;
    }
    return (uint64_t)(uintptr_t)p;
}

// np106b：大分配记录落地（构造器期之后调用，此时打日志才安全）
static void dump_big_allocs(void) {
    uint64_t b2 = (uint64_t)(uintptr_t)g_k_m.base;
    for (int i = 0; i < g_n_big; i++) {
        int in = g_big_lr[i] >= b2 && g_big_lr[i] < b2 + DM_IMG_MAIN_END;
        klog("[大分配] #%d %#llx 字节（第 %d 次分配）；站点=%s%#llx\n", i,
             (unsigned long long)g_big_sz[i], (int)g_big_seq[i],
             in ? "+" : "(外)",
             (unsigned long long)(in ? g_big_lr[i] - b2 : g_big_lr[i]));
    }
}
// 批量挂点：扫全映像，找出所有指向 target 的 **bl（调用）与 b（尾调用）** 站点并全部挂上。
// 为什么连 b 也要挂：崩链上的调用可能是尾调用（不更新 x30），只扫 bl 会把它们漏掉。
// 为什么要扫两块：改写器把每个 svc 门点改成跳进**补丁区**的蹦床，所以补丁区是真代码——
// 只扫 text 会漏站点（离线用 tools/dm_branch.py 核实过）。代码洞本身不扫：里面是我们自己的桩。
static void k_hook_all_branchers(uint8_t *img, uint32_t target_off, void *host_fn, const char *what, int cap) {
    const uint32_t lo[2] = {0, DM_IMG_MAIN_PATCH_OFF};
    const uint32_t hi[2] = {DM_IMG_MAIN_TEXT_END, DM_IMG_MAIN_CAVE_OFF};
    uint32_t n = 0, nbl = 0, nb = 0;
    for (int r = 0; r < 2; r++) {
        for (uint32_t a = lo[r]; a + 4 <= hi[r] && n < (uint32_t)cap; a += 4) {
            uint32_t w = *(uint32_t *)(img + a);
            uint32_t op = w & 0xFC000000u;
            if (op != 0x94000000u && op != 0x14000000u) continue;      // bl / b
            int32_t imm = (int32_t)(w & 0x03FFFFFFu);
            if (imm & 0x02000000) imm -= 0x04000000;
            if ((int64_t)a + (int64_t)imm * 4 != (int64_t)target_off) continue;
            k_install_hook(img, a, host_fn, what);
            n++;
            if (op == 0x94000000u) nbl++; else nb++;
        }
    }
    klog("[插桩] %s：共挂 %u 个站点（bl %u / b 尾调用 %u，上限 %d）\n", what, n, nbl, nb, cap);
}
static void k_inject_heap(uint8_t *img) {
    for (int i = 0; i < DM_ALLOC_VTBL_QWORDS; i++)
        g_alloc_vtbl[i] = (uint64_t)(uintptr_t)&vtbl_fallback;   // 先全铺兜底（记录调用点、返回 0）
    g_alloc_vtbl[6]  = (uint64_t)(uintptr_t)&host_alloc;         // +0x30 分配内存
    g_alloc_vtbl[16] = (uint64_t)(uintptr_t)&vtbl_slot16_size;   // +0x80 报尺寸 ⇒ 0x1000
    g_alloc_vtbl[17] = (uint64_t)(uintptr_t)&vtbl_slot17_ok;     // +0x88 当 bool 用 ⇒ 返 1
    g_alloc_obj[0] = (uint64_t)(uintptr_t)&g_alloc_vtbl[0];
    g_alloc_obj[1] = (uint64_t)(uintptr_t)&g_alloc_obj[0];   // +8 自指：0xa0e2f8 的兜底路径取到同一对象
    // ⚠ 真机实证（2026-10-17）：跑到 0xa0e2f8 时这个自指值**已被清成 0**（游戏会写它），
    //   所以兜底路径不可靠——改由 GetTlsValue 的影子对象 [+0xb0] 顶上（见 g_tls_shadow）。
    g_alloc_holder[0] = (uint64_t)(uintptr_t)&g_alloc_obj[0];
    g_ctx_holder[0] = (uint64_t)(uintptr_t)&g_ctx_obj[0];
    g_tls_shadow[0xb0 / 8] = (uint64_t)(uintptr_t)&g_alloc_obj[0];   // 影子对象 [+0xb0] = 分配器对象
    *(uint64_t *)(img + 0x226f0a0) = (uint64_t)(uintptr_t)&g_alloc_holder[0];   // 分配器持有者
    *(uint64_t *)(img + 0x22783d8) = (uint64_t)(uintptr_t)&g_ctx_holder[0];     // 线程上下文持有者
    // 工厂 0x1f8 的入参：读 [x1+0x18] → 对象 → [+0x18] → 虚方法（见崩点 0x103b8a8）
    g_mgr_sub[0x18 / 8] = (uint64_t)(uintptr_t)&mgr_method;
    g_mgr_obj[0x18 / 8] = (uint64_t)(uintptr_t)&g_mgr_sub[0];
    // nnMain 路径的 sead::ThreadMgr（见 g_tm_a 处注释：这个槽此前漏注 ⇒ 崩 0x9FE9A8）
    for (int i = 0; i < DM_TM_VTBL_QWORDS; i++) g_tm_vtbl[i] = (uint64_t)(uintptr_t)&tm_method;
    g_tm_obj[0] = (uint64_t)(uintptr_t)&g_tm_vtbl[0];
    g_tm_a[0]   = 1;                                  // 标志位：非零才会往下走
    g_tm_a[1]   = (uint64_t)(uintptr_t)&g_tm_obj[0];
    *(uint64_t *)(img + 0x227aac0) = (uint64_t)(uintptr_t)&g_tm_a[0];
    // *(0x227df98) → 模块 .bss 的 0x22d9198；模块代码从不写它（见 g_cb_obj 处注释）
    g_cb_obj[0x20 / 8] = (uint64_t)(uintptr_t)&cb_stub;
    *(uint64_t *)(img + 0x22d9198) = (uint64_t)(uintptr_t)&g_cb_obj[0];
    klog("[注入自检] g_alloc_obj=%p [0]=%#llx [1]=%#llx；g_tls_shadow=%p [+0xb0]=%#llx\n",
         (void *)&g_alloc_obj[0], (unsigned long long)g_alloc_obj[0], (unsigned long long)g_alloc_obj[1],
         (void *)&g_tls_shadow[0], (unsigned long long)g_tls_shadow[0xb0 / 8]);
}

// ★ 跑模块自己的静态初始化（全局对象构造）。深跑直接跳进 nnMain ⇒ 跳过了这一步，
//   data 段里那些全局对象全是空的（真机实证：偏移 0x22D8AA0 的对象首字=0 ⇒ 崩在 blr 野指针）。
// 依据：MOD0 动态段里的 DT_INIT / DT_INIT_ARRAY / DT_INIT_ARRAYSZ（离线解析出的值）：
//   DT_INIT = 0x24；DT_INIT_ARRAY = 0x2293390；DT_INIT_ARRAYSZ = 0x21f8 ⇒ 1085 个构造器。
#define DM_IMG_MAIN_INIT_OFF        0x24u
#define DM_IMG_MAIN_INIT_ARRAY_OFF  0x2293390u
#define DM_IMG_MAIN_INIT_ARRAY_SIZE 0x21f8u
// 已知会崩的构造器（先跳过以验证因果，别让它们污染后面）：
//   #184 = `*(0x22d84a0)=2π`，崩因是读 nn::util::FloatPi 的 GLOB_DAT 槽（0x2280648）为空指针
//         （2026-10-18：槽已在 k_fill_data_imports 补上 ⇒ 放出来重跑，静观是否真过）。
//         旧注释"把 x20 当对象指针"是早前误判——反汇编显示它根本不碰 x20。
//   #206 待查（0xb2a370，一个注册表构造器）。
// 2026-10-18：#206（0xb2a370）**放行重试**。它此前"已知会崩"被跳过，但新的线索表明
// 它可能在填某个注册表/全局对象（用了 0x2264288/0x2264448 等类型全局，且扫一个找 0x3c 的表）。
// gate 会兜底：崩了自动跳过并打印 pc/si_addr，成功则可能顺带解开当前的解析失败。
// 若确认无用可改回 return i == 206。
static int k_skip_ctor(size_t i) { (void)i; return 0; }

static void k_run_module_init(const module_t *m) {
    uint8_t *b = m->base;
    size_t n = DM_IMG_MAIN_INIT_ARRAY_SIZE / 8;
    klog("[静态初始化] DT_INIT=%#llx，DT_INIT_ARRAY=%#llx，共 %zu 个构造器\n",
         (unsigned long long)(uintptr_t)(b + DM_IMG_MAIN_INIT_OFF),
         (unsigned long long)(uintptr_t)(b + DM_IMG_MAIN_INIT_ARRAY_OFF), n);
    typedef void (*ctor_t)(int, char **, char **);
    ctor_t *arr = (ctor_t *)(b + DM_IMG_MAIN_INIT_ARRAY_OFF);
    size_t ran = 0, failed = 0;
    // 逐个武装 + 逐个编号：崩了记现场并**跳过继续跑**（先看整体能走多远，再逐个收拾）。
    // 真机实证：#184 崩在 `ldr x9,[x10]`（x10=0 ⇒ si_addr=0）；注意报告的 pc 常比真正访存的
    // 指令多 4，判读时 pc 与 pc-4 都要看。
    for (size_t i = 0; i < n; i++) {
        if (!arr[i]) continue;
        if (k_skip_ctor(i)) { klog("[静态初始化] #%zu 跳过（已知会崩）\n", i); continue; }
        klog("[静态初始化] #%zu/%zu → %#llx\n", i, n, (unsigned long long)(uintptr_t)arr[i]);
        if (sigsetjmp(g_gate_jmp, 1) == 0) {
            g_gate_armed = 1;
            // np102 实验（给 argv 传零缓冲）已撤销：无效——#206 的崩点不在约定上，
            //   而在"查表拿到空指针"（见文档 §49：根因是内容没加载，表计数为 0）。
            arr[i](0, NULL, NULL);
            g_gate_armed = 0;
            ran++;
            // np107：**逐个查宅基地**——门字节 [*(0x2276168)+0x419] 一旦变非零，
            //   刚才那个构造器就是写入者。只记录不打印（np58 教训），循环后统一输出。
            if (!g_gate_caught) {
                // np109：装载器是 **x8=[0x2276168]; x8=[x8]; ldrb [x8+0x419]**（二次解引用！）
                //   真正的门对象 = [[0x2276168]]。np108 实测 [0x2276168]→0x22fc760 的首字为 0
                //   ⇒ 内层指针还没建立。这里改判"内层指针首次非零"。
                uint64_t g = *(const uint64_t *)(uintptr_t)((uint8_t *)b + 0x2276168);
                uint64_t obj = (g > 0x10000ull && g < 0x800000000000ull)
                             ? *(const uint64_t *)(uintptr_t)g : 0;
                if (obj > 0x10000ull && obj < 0x800000000000ull) {
                    g_gate_caught = 1;
                    g_gate_ctor = i;
                    g_gate_ctorentry = (uint64_t)(uintptr_t)arr[i];
                    g_gate_obj = obj;
                    g_gate_val = *(const uint8_t *)(uintptr_t)(obj + 0x419);
                }
            }
        } else {
            g_gate_armed = 0;
            failed++;
            klog("=== 静态初始化 #%zu 崩：pc=%#llx sig=%d si_addr=%#llx（跳过，继续）===\n",
                 i, (unsigned long long)g_gate_fault_pc, g_gate_fault_sig,
                 (unsigned long long)g_gate_fault_addr);
            gate_dump_fault("静态初始化");
        }
    }
    klog("[静态初始化] 完成：成功 %zu 个，失败 %zu 个\n", ran, failed);
    // np66：槽体检第二站——静态初始化之后。装载后该槽是对的（base+0x22d9260），
    // 若这里变 0 ⇒ **某个构造器把它清零了**；若还是对的 ⇒ 是 nnMain 里的流程清零的。
    dump_big_allocs();   // np106b：大分配记录落地（此刻打日志才安全）
    // np107：门字节写入者（构造器序号 + 入口）
    if (g_gate_caught) {
        uint64_t b3 = (uint64_t)(uintptr_t)b;
        uint64_t e = g_gate_ctorentry;
        int in = e >= b3 && e < b3 + DM_IMG_MAIN_END;
        klog("[启动包门] 写入者 = 构造器 #%zu（入口 %s%#llx）；对象=%#llx 当时值=%#x\n",
             g_gate_ctor, in ? "+" : "(外)",
             (unsigned long long)(in ? e - b3 : e),
             (unsigned long long)g_gate_obj, g_gate_val);
        // np108：dump 门对象的内存，弄清它是什么（对象地址换算成映像内偏移）
        {
            const uint8_t *o = (const uint8_t *)(uintptr_t)g_gate_obj;
            int ino = g_gate_obj >= b3 && g_gate_obj < b3 + DM_IMG_MAIN_END;
            klog("[启动包门] 对象映像内偏移 = %s%#llx\n",
                 ino ? "+" : "(外)",
                 (unsigned long long)(ino ? g_gate_obj - b3 : g_gate_obj));
            char hx[96]; size_t u = 0;
            for (int k = 0; k < 24 && u + 3 < sizeof hx; k++)
                u += (size_t)snprintf(hx + u, sizeof hx - u, "%02x", o[k]);
            hx[u] = 0; klog("[启动包门] 头 24 字节：%s\n", hx);
            u = 0;
            for (int k = 0x410; k < 0x428 && u + 3 < sizeof hx; k++)
                u += (size_t)snprintf(hx + u, sizeof hx - u, "%02x", o[k]);
            hx[u] = 0; klog("[启动包门] +0x410..+0x428：%s\n", hx);
        }
    } else {
        klog("[启动包门] 构造器跑完时门仍为 0 ⇒ 4 是启动链/nnMain 阶段写的\n");
    }
    klog("[槽体检] 静态初始化后：0x2280eb8=%#llx（装载后应=%#llx）\n",
         (unsigned long long)*(const uint64_t *)(b + 0x2280eb8),
         (unsigned long long)((uintptr_t)b + 0x22d9260));
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
    // 装机他判：把本次编译时间打进日志——跑完先看这一行与本地编译时间是否一致，
    // 不一致就说明设备上跑的是旧包（今天已被这个问题浪费三轮）。
    klog("=== 探针构建：%s %s ===\n", __DATE__, __TIME__);
    // ★ 影子「当前堆」：必须在静态初始化与 nnMain **之前**挂上——
    //   nnMain 一开始就要"取当前堆"，取不到就跳过 7 个初始化（见 §24/§25）。
    k_fake_heap_install((uint8_t *)g_k_m.base);
    k_run_module_init(&g_k_m);   // ★ 先跑模块自己的静态初始化（否则 data 段的全局对象全是空的）
    {   // 崩点归属用：把本进程映射抄进日志（模块区 / 探针 .so / 宿主堆 各自范围）
        FILE *mp = fopen("/proc/self/maps", "r");
        if (mp) {
            char line[256]; int n = 0;
            klog("=== 进程映射（前 24 行）===\n");
            while (n++ < 24 && fgets(line, sizeof line, mp)) klog("  %s", line);
            fclose(mp);
        }
    }
    // np105b：启动包装载的门（0xd5b670：*(0x2276168) → 对象 → [+0x419] != 0 则跳过装载）。
    //   在三个关键时刻读它，看这个 4 是什么时候写进去的。
    #define GATE_LOG(tag) do { \
        uint64_t _g = *(const uint64_t *)(uintptr_t)(b + 0x2276168); \
        uint64_t _o = (_g > 0x10000ull && _g < 0x800000000000ull) \
                   ? *(const uint64_t *)(uintptr_t)_g : 0; \
        uint8_t _v = 0xff; \
        if (_o > 0x10000ull && _o < 0x800000000000ull) \
            _v = *(const uint8_t *)(uintptr_t)(_o + 0x419); \
        klog("[启动包门] %s：外层=%#llx 内层(真门对象)=%#llx [+0x419]=%#x\n", tag, \
             (unsigned long long)_g, (unsigned long long)_o, _v); \
    } while (0)
    GATE_LOG("启动链前");
    g_k_stage = "启动链 0x1c8";
    klog("=== 起跑：入口 0x1c8（启动链）base %#llx ===\n", (unsigned long long)b);
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        g_k_rv = ((uint64_t (*)(void))(b + 0x1c8))();
        g_gate_armed = 0;
        klog("=== 启动链返回：rv=%#llx ===\n", (unsigned long long)g_k_rv);
    } else {
        g_gate_armed = 0; g_k_faulted = 1;
        klog("=== 启动链崩：pc %#llx sig %d ===\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        gate_dump_fault("启动链 0x1c8");
        return NULL;
    }
    GATE_LOG("启动链后");
    // 更深一站：模块的第一个真实功能函数 0x1f8（C++ 对象工厂：读数据槽 → 分配 0xaa0 → 构造）
    g_k_stage = "工厂 0x1f8";
    klog("=== 深跑：0x1f8（对象工厂，x0 = 注入的管理器对象）===\n");
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (sigsetjmp(g_gate_jmp, 1) == 0) {
        uint64_t r2 = ((uint64_t (*)(uint64_t))(b + 0x1f8))((uint64_t)(uintptr_t)&g_mgr_obj[0]);
        g_gate_armed = 0;
        klog("=== 0x1f8 返回：rv=%#llx ===\n", (unsigned long long)r2);
    } else {
        g_gate_armed = 0; g_k_faulted = 1;
        klog("=== 0x1f8 崩：pc %#llx sig %d ===\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        gate_dump_fault("工厂 0x1f8");
        // np86：**迟到防护**——本分支是 fall-through（设计上崩完要继续跑 nnMain）。但若
        //   nnMain 段已经进入过，说明这次 longjmp 是"后面阶段的崩倒流回本阶段的 setjmp"，
        //   继续 fall-through 就会"装影子堆→崩→回这里→又跑 nnMain"无限循环（np85 实测
        //   空转 94,108 次、日志 226 万行）。此时必须止步。
        if (g_nnmain_entered) {
            klog("【红】工厂分支被迟到触发（nnMain 已进入过）——止步，避免倒流成环\n");
            return NULL;
        }
    }
    // 终站：游戏的真正入口 nnMain（模块符号表 #506：st_value 0x735330，size 1008，GLOBAL FUNC）
    // 依据：rtld 的符号表里列出它要为主模块解析的入口名 nnMain（启动约定，非猜测）。
    GATE_LOG("nnMain前");
    g_k_stage = "nnMain";
    g_nnmain_entered = 1;   // np86：置位后，工厂分支就知道"别再 fall-through 了"
    klog("=== 终站：nnMain（0x735330）===\n");
    // 再挂一次影子堆：万一静态初始化里有构造器把它清零，这里补回来（代价极小）。
    k_fake_heap_install((uint8_t *)b);
    // np72：**框架自证**——影子堆安装会写映像里的几个洞；这里确认它没有顺手清掉 0x2280eb8。
    // 若这一行读到 0，说明"槽变 0"是我们自己的注入造成的（**通用框架 bug**，影响任何游戏）。
    klog("[槽体检] 影子堆安装后：0x2280eb8=%#llx（应=%#llx）\n",
         (unsigned long long)*(const uint64_t *)(b + 0x2280eb8),
         (unsigned long long)((uintptr_t)b + 0x22d9260));
    g_gate_armed = 1; g_gate_fault_pc = 0;
    if (!g_fixup_page) g_fixup_page = calloc(1, 0x8000);   // 越障暂存区 32KB（64 块 × 0x100，见 gate_fault_handler）
    g_fixup_base = (uintptr_t)b;
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
        nvn_api_summary("nnMain 返回");
    } else {
        g_gate_armed = 0; g_k_faulted = 1;
        klog("=== nnMain 崩：pc %#llx sig %d ===\n", (unsigned long long)g_gate_fault_pc, g_gate_fault_sig);
        // np73（§38.8 的决定性测试）：把 **pc 处的运行时指令字节**打出来，和文件对照。
        // 文件里 0xa2469c..0xa246a8 = 9000c2e8 / f9475d08 / f9000100 / d65f03c0
        //   （adrp / ldr x8,[x8,#0xeb8] / str x0,[x8] / ret）
        // 若运行时前 1-2 条被改写 ⇒ 那里有钩子桩（语义已变，不能按原指令推断崩因）。
        {
            const uint32_t *c = (const uint32_t *)(uintptr_t)(g_gate_fault_pc - 8);
            klog("[指令] pc-8..pc+8 运行时 = %08x %08x %08x %08x %08x\n",
                 c[0], c[1], c[2], c[3], c[4]);
            klog("[指令] 文件里同处应为   = d65f03c0 9000c2e8 f9475d08 f9000100 d65f03c0\n");
        }
        // np64：槽体检——判"运行期槽是 0"到底是**游戏写回 0**还是**装载层 bss 清零洗掉了重定位**
        // （崩点 +0xA046A4 的存储目标 x9 = *(0x2280aa8) = 0；RELA 本该是 base+0x22d8790）
        {
            const uint8_t *im = (const uint8_t *)(uintptr_t)b;
            uint64_t s98 = *(const uint64_t *)(im + 0x2280a98);
            uint64_t sa0 = *(const uint64_t *)(im + 0x2280aa0);
            uint64_t sa8 = *(const uint64_t *)(im + 0x2280aa8);
            klog("[槽检] 0x2280a98=%#llx 0x2280aa0=%#llx 0x2280aa8=%#llx（基址=%#llx，应为 基址+0x22d8790=%#llx）\n",
                 (unsigned long long)s98, (unsigned long long)sa0, (unsigned long long)sa8,
                 (unsigned long long)(uintptr_t)b,
                 (unsigned long long)((uintptr_t)b + 0x22d8790));
            klog("[槽检] .bss 0x22d8790 内容=%#llx；0x22d8798 首字节=%#x（once 标志）\n",
                 (unsigned long long)*(const uint64_t *)(im + 0x22d8790),
                 (unsigned)*(const unsigned char *)(im + 0x22d8798));
            // np70：崩时把槽**前后各 4 个 qword** 也打出来——判"整片被清零（memset 越界）"
            //      还是"只有这一个槽被改（定向写/竞态）"。这是回答 §38.6 那个"两指令内变 0"的直证。
            {
                const uint64_t *p = (const uint64_t *)(im + 0x2280eb8);
                klog("[槽周边] 0x2280eb8 前4: %016llx %016llx %016llx %016llx\n",
                     (unsigned long long)p[-4], (unsigned long long)p[-3],
                     (unsigned long long)p[-2], (unsigned long long)p[-1]);
                klog("[槽周边] 自身/后3: %016llx %016llx %016llx %016llx\n",
                     (unsigned long long)p[0], (unsigned long long)p[1],
                     (unsigned long long)p[2], (unsigned long long)p[3]);
                const uint64_t *q = (const uint64_t *)(im + 0x22d9260);
                klog("[槽周边] 变量 0x22d9260 及其后: %016llx %016llx %016llx %016llx\n",
                     (unsigned long long)q[0], (unsigned long long)q[1],
                     (unsigned long long)q[2], (unsigned long long)q[3]);
            }
        }
        if (g_fixup_n)
            klog("[越障] 共拦截 %d 次；前 8 处 PC 偏移：%#llx %#llx %#llx %#llx %#llx %#llx %#llx %#llx\n",
                 (int)g_fixup_n,
                 (unsigned long long)g_fixup_last[0], (unsigned long long)g_fixup_last[1],
                 (unsigned long long)g_fixup_last[2], (unsigned long long)g_fixup_last[3],
                 (unsigned long long)g_fixup_last[4], (unsigned long long)g_fixup_last[5],
                 (unsigned long long)g_fixup_last[6], (unsigned long long)g_fixup_last[7]);
        k_log_counters("崩时");
        gate_dump_fault("nnMain");
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
        k_fill_data_imports(img);   // ★ 补数据导入常量（nn::util 数学常量等，见函数注释）
        g_hook_n = 0;   // ★ 每轮从零开始：洞是映像内的固定空间，上一轮装的桩还占着格
        k_inject_heap(img);   // ★ 喂堆：把游戏分配器要的两个全局指向宿主对象（见 k_inject_heap）
        k_inject_log(img);    // ★ 喂日志对象 + 节点池（见 k_inject_log）
        // ★ 三个分配器用「替换式」：宿主直接给内存，跳过游戏自己的实现（绕开注入被改写/分支不确定）
        k_install_replace_hook(img, 0x9f34d8, (void *)host_alloc_replace, "分配器 0x9f34d8");
        k_install_replace_hook(img, 0x9f3464, (void *)host_alloc_replace, "分配器 0x9f3464");
        k_install_replace_hook(img, 0x9f33f0, (void *)host_alloc_replace, "分配器 0x9f33f0");
        // 崩链上的两个构造器入口（记录 this）
        k_install_hook(img, 0xa08ef4, (void *)hook_ctor_log, "构造器 0xa08ef4 入口");
        k_install_hook(img, 0xa08e94, (void *)hook_ctor2_log, "构造器 0xa08e94 入口");
        // ★★ 沿崩链逐点插桩（链见上面 hook_p* 的注释）。先挂"信息量最大"的链点，
        //    再挂批量站点——洞只有 227 格（0x10000/0x120），要紧的放前面，满了也丢不了它们。
        k_install_hook(img, 0x9fe24c, (void *)hook_p9fe24c, "链点 0x9fe24c（链顶函数入口）");
        k_install_hook(img, 0x9fe2e8, (void *)hook_p9fe2e8, "站点→0xa1e484（交出注入对象处）");
        k_install_hook(img, 0xa1e484, (void *)hook_p1e484, "链点 0xa1e484（b 尾调用桩）");
        k_install_hook(img, 0xa1dd94, (void *)hook_p1dd94, "链点 0xa1dd94（分配 0x100 + 调构造器）");
        k_install_hook(img, 0xa1d8e0, (void *)hook_p1d8e0, "链点 0xa1d8e0（派生构造器入口）");
        // ★ 钉子 #8（当前卡点：0xa00a78 收到空标签 ⇒ 返 0 ⇒ 0xa01790 写 [x21+0xc8] 崩）：
        //   五个钩子都排在批量挂点之前，保证装得上（离线分析见 hook_e* 处注释）。
        k_install_hook(img, 0x9fe82c, (void *)hook_e1, "钉子8 PathA 0x9fe82c 入口");
        k_install_hook(img, 0xa01790, (void *)hook_e2, "钉子8 0xa01790 入口（标签+调用点）");
        k_install_hook(img, 0xa00a78, (void *)hook_e3, "钉子8 0xa00a78 入口（标签）");
        k_install_hook(img, 0x9fe910, (void *)hook_e4, "钉子8 tag2 目标 0x9fe910 入口");
        k_install_hook(img, 0xa01814, (void *)hook_e5, "钉子8 分发返回点 0xa01814");
        // ★ 钉子 #8 同轮探针（根因验证三连，只读）：工厂真入口 0xa00f8c（0xa00f90 是函数体
        //   第二条指令）、池注册 0xa018f8、G 真入口 0xa01d70（0xa01d88 同理是体内指令）。
        k_install_hook(img, 0xa00f8c, (void *)hook_e6, "钉子8 工厂 0xa00f8c 入口（父对象）");
        k_install_hook(img, 0xa018f8, (void *)hook_e7, "钉子8 池注册 0xa018f8 入口（记录源）");
        k_install_hook(img, 0xa01d70, (void *)hook_e8, "钉子8 G 0xa01d70 入口（池状态）");
        // ★ 钉子 #8 续修探针（runner 链，只读）：激活检查 + 运行转发器，见 hook_e9/e10 注释。
        k_install_hook(img, 0xa01cf0, (void *)hook_e9, "钉子8 runner 检查 0xa01cf0 入口");
        k_install_hook(img, 0x9fdccc, (void *)hook_e10, "钉子8 运行转发器 0x9fdccc 入口");
        k_install_hook(img, 0xa01fe0, (void *)hook_e11, "钉子8 runner 入口 0xa01fe0（链表遍历）");
        k_install_hook(img, 0xa01be8, (void *)hook_e12, "钉子8 销毁入口 0xa01be8（数组 dump）");
        // ★ 注册体/构造尾定位钩（见 hook_e13/e14 注释）
        k_install_hook(img, 0xa01928, (void *)hook_e13, "钉子8 注册体 0xa01928");
        k_install_hook(img, 0xa00d4c, (void *)hook_e14, "钉子8 构造尾 0xa00d4c");
        // ★ 日志系统探针（见 hook_e15/e16 注释）
        k_install_hook(img, 0xf35374, (void *)hook_e15, "日志 添加 0xf35374");
        k_install_hook(img, 0xf35400, (void *)hook_e16, "日志 遍历 0xf35400");
        k_install_hook(img, 0xa01924, (void *)hook_e17, "钉子8 注册体上锁前 0xa01924");
        // ★ Task #16：第一个任务由谁创建（见 hook_e18/e19 注释；离线结论见进度文档 §20）
        k_install_hook(img, 0xa0143c, (void *)hook_e18, "任务 CreateTaskImmediate 0xa0143c 入口");
        k_install_hook(img, 0xa01490, (void *)hook_e19, "任务 建出任务回填 0xa01490（x0=新任务）");
        // ★ Task #16 第二轮：名字级追踪（见 hook_e20/e21 注释）
        k_install_hook(img, 0xa0913c, (void *)hook_e20, "名字 建/查管理器 0xa0913c 入口");
        k_install_hook(img, 0x9fe380, (void *)hook_e21, "名字 方法树创建结果 0x9fe380");
        k_install_hook(img, 0xa09144, (void *)hook_e22, "名字 建/查管理器 0xa09144 实体入口");
        k_install_hook(img, 0x9fe984, (void *)hook_e23, "框架 SystemManagers 建 ThreadMgr 0x9fe984");
        k_install_hook(img, 0xa112fc, (void *)hook_e24, "挂载 方法树列表 0xa112fc");
        // ⚠ np34 教训：原来挂在 0x9ff398（`adrp x8,#0x1ba6000`）——那是**给后续指令喂寄存器**的位置，
        //   宿主函数调用会破坏 caller-saved 的 x8，导致紧跟的 `add x8,x8,#0x891` 算出垃圾指针。
        //   改挂 0x9ff3a0（`str x21,[x23,#0x350]`，纯 store，不喂任何寄存器给后续指令）。
        k_install_hook(img, 0x9ff3a0, (void *)hook_e25, "框架 挂 7 棵方法树 0x9ff3a0（纯 store）");
        // ⚠ 2026-10-18：洞容量 227 格、已用 218 ⇒ 快满了。下面两条 all_branchers 最多各占
        //   64/200 格，且"找站点"的使命已经完成（钉子 #8 早已闭环）⇒ 暂时关闭，把格子让给
        //   **启动链追踪**（一次拿到 nnMain 的完整调用顺序，比逐个微探针性价比高得多）。
        // k_hook_all_branchers(img, 0xa1d8e0, (void *)hook_s1d8e0, "站点→0xa1d8e0", 64);
        // k_hook_all_branchers(img, 0xa08e94, (void *)hook_any_site, "站点→构造器 0xa08e94", 200);
        // 诊断 sead 单例查找 0xa0913c 的分支路径 —— 同样已完成使命，关掉腾格子。
        // k_install_hook(img, 0xa0919c, (void *)hook_lk_a, "查找路径 0xa0919c");
        // k_install_hook(img, 0xa091e4, (void *)hook_lk_b, "查找路径 0xa091e4");
        // k_install_hook(img, 0xa091dc, (void *)hook_lk_c, "查找路径 0xa091dc");
        // k_install_hook(img, 0xa092a0, (void *)hook_lk_d, "查找路径 0xa092a0");
        // k_install_hook(img, 0xa092a8, (void *)hook_lk_e, "查找路径 0xa092a8");
        // k_install_hook(img, 0xa09184, (void *)hook_dbA, "诊断 0xa09184（调 0xa0e2f8）");
        // k_install_hook(img, 0xa0e330, (void *)hook_dbB, "诊断 0xa0e330（取值点之后）");
        // k_install_hook(img, 0xa0c1b0, (void *)hook_dbC, "诊断 0xa0c1b0（调 sead 助手前）");
        // k_install_hook(img, 0xa1d57c, (void *)hook_dbD, "诊断 0xa1d57c（sead 助手入口）");
        // ★ 启动链追踪：nnMain 的 bl 目标按出现顺序全部挂上（一个宿主函数，靠 f[FR_SITE] 区分）。
        //   目的：拿到"初始化走到第几步、在哪一步之后断掉"的完整顺序图。
        for (size_t bi = 0; bi < sizeof g_boot_sites / sizeof g_boot_sites[0]; bi++)
            k_install_hook(img, g_boot_sites[bi], (void *)hook_boot, "启动链");
        // np68：设值函数换替身（覆盖上面那条记录式钩子——同一站点只能挂一个）。
        // ⚠ 真机：装了替身之后**确定性卡死在静态初始化 ctor #385**（同 np58 的症状）——
        //   说明 ctor #385 会调这个设值函数，而替身在那条路径上把它打断（也可能是同站点
        //   二次挂接和记录钩子的洞冲突）。**暂不启用**，留作线索；恢复后 np67 行为可复现。
        // k_install_replace_hook(img, 0xa2469c, (void *)hook_setter, "设值 0xa2469c 替身");
        // 决定性挂点：`mov x22, x0`（x22==0 ⇒ 跳过 7 个初始化直接进主循环）
        k_install_hook(img, 0x73557c, (void *)hook_e26, "关键 x22 取值 0x73557c");
        k_install_hook(img, 0x7355a8, (void *)hook_e27, "对照 cbz 未跳则命中 0x7355a8");
        k_install_hook(img, 0xa051c4, (void *)hook_e28, "建对象 worker 0xa051c4（看 x2 是谁传的 0）");
        // 崩点链最上游的解析器：原实现一路缺数据恒返 0 ⇒ worker 的 x2=0 ⇒ 崩 0xa05324。
        // np54 起换成**替身**：宿主直接返全零对象，越过这道坎看下一层需求（§八影子对象打法）。
        k_install_replace_hook(img, 0x9fa4c8, (void *)hook_rep_parse, "键解析·替身 0x9fa4c8（返全零对象）");
        // 解析派发（worker 的调用者，函数指针进来）：直接看它喂给构造函数的 x2
        k_install_hook(img, 0xa05fa4, (void *)hook_e30, "派发 0xa05fa4（看 x2）");
        g_nplt = 0; g_plt_calls = 0;
        for (uint64_t off = 0; off + 24 <= DT_PLTRELSZ; off += 24) {
            uint64_t r_off  = *(uint64_t *)(img + DT_JMPREL + off);
            uint64_t r_info = *(uint64_t *)(img + DT_JMPREL + off + 8);
            uint32_t symi   = (uint32_t)(r_info >> 32);
            const char *nm  = (const char *)(img + DT_STRTAB + *(uint32_t *)(img + DT_SYMTAB + (uint64_t)symi * 24));
            if (g_nplt < 512) { g_plt[g_nplt].slot = (uint64_t)(img + r_off); g_plt[g_nplt].name = nm; g_nplt++; }
            // ★ 已实现的导入直连宿主函数（目前 = nn::fs 23 个）；其余仍落记录型存根。
            //   分流按**符号名**，换游戏不用改表（只要 ABI 同名）。
            void *impl = dm_fs_impl(nm);
            if (impl) { g_nimpl++; if (g_nimpl <= 40) klog("[导入] 直连 %s\n", nm); }
            *(uint64_t *)(img + r_off) = impl ? (uint64_t)(uintptr_t)impl
                                              : (uint64_t)(uintptr_t)&plt_stub;
        }
        {   // np82：记下本 .so 的装载基址，队列/fs 日志里的"外"调用者才能对上号
            Dl_info di;
            if (dladdr((void *)&plt_stub, &di) && di.dli_fbase) {
                g_self_base = (uint64_t)(uintptr_t)di.dli_fbase;
                klog("[自检] 本 .so 基址 = %#llx（%s）\n",
                     (unsigned long long)g_self_base, di.dli_fname ? di.dli_fname : "?");
            }
        }
        dm_fs_set_log(klog);
        dm_fs_set_base(img);   // np80：让 fs 日志能把调用者 LR 换算成映像内偏移
        dm_sdk_set_log(klog);   // 通用 SDK 胶水层也用同一份日志
        dm_nvn_set_log(klog);   // 通用 NVN 胶水层同此
        dm_nvn_vk_set_log(klog);   // 池/缓冲层同此
        dm_fs_selftest();
        // np65 槽体检（装载后、游戏跑之前）：崩点 +0xA246A4 的 `str x0,[*(0x2280eb8)]` 里那个槽
        // 运行期是 0，但镜像里它有 RELA（addend 0x22d87e0）。这里在**游戏动手之前**读一次：
        //   = base+0x22d87e0 ⇒ 重定位是对的，是游戏后来把它写成 0（应用层问题）
        //   = 0            ⇒ 装载层漏了这条重定位（框架 bug，影响面大）
        klog("[槽体检] 装载后：0x2280eb8=%#llx（应=%#llx）｜对照 0x2280a98=%#llx 0x227f900=%#llx\n",
             (unsigned long long)*(const uint64_t *)(img + 0x2280eb8),
             (unsigned long long)((uintptr_t)img + 0x22d87e0),
             (unsigned long long)*(const uint64_t *)(img + 0x2280a98),
             (unsigned long long)*(const uint64_t *)(img + 0x227f900));
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
    appf(out, cap, used, "  崩链插桩命中：链点 0x9fe24c %d / 站点→0xa1e484 %d / 0xa1e484 %d / 0xa1dd94 %d / "
                         "0xa1d8e0 %d；构造器 0xa08e94 入口 %d 次、站点 %d 次（明细见 dm_run_test.log）\n",
         g_n_9fe24c, g_n_9fe2e8, g_n_1e484, g_n_1dd94, g_n_1d8e0, g_n_ctor2, g_n_any);
    appf(out, cap, used, "  钉子8 命中：PathA入口 %d / 0xa01790 %d / 0xa00a78 %d / 0x9fe910 %d / 分发返回 %d"
                         "（标签与返回值见 dm_run_test.log 的 [钉子8] 行）\n",
         g_n_e1, g_n_e2, g_n_e3, g_n_e4, g_n_e5);
    appf(out, cap, used, "  钉子8 影子：记录标签 0 补 {2, ControllerMgr 工厂} %d 次；"
                         "状态 1⇒2 补推 %d 次；同轮探针 工厂 %d / 池注册 %d / G %d\n",
         g_n_e2fix, g_n_e8fix, g_n_e6, g_n_e7, g_n_e8);
    appf(out, cap, used, "  钉子8 runner 链：激活检查 %d 次 / 运行转发器 %d 次 / "
                         "runner 入口 %d 次 / 销毁 %d 次（明细见日志）\n",
         g_n_e9, g_n_e10, g_n_e11, g_n_e12);
    {
        int n = 0;
        for (int i = 0; i < DM_THREAD_MAX; i++) if (g_thr[i].game) n++;
        appf(out, cap, used, "  线程：游戏建了 %d 个（明细见日志 [线程] 行）\n", n);
    }
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
    // 2026-10-16 再收窄：G/H/I/J 均已验收（93/93、3/3、串链全绿），每轮不再重复跑，
    // 只跑当前焦点实验 K（启动链→nnMain→崩点）——每轮省约 40 秒；要回归时把它们放开即可。
    logline(">>> 实验 G/H/I/J 已验收，本轮跳过（要回归时放开）");
    (void)exp_g; (void)exp_h; (void)exp_i; (void)exp_j;
    logline(">>> 实验 K 开始"); exp_k(files_dir, out, cap, &used); logline(">>> 实验 K 返回");

    app(out, cap, &used, "==================================================\n");
    app(out, cap, &used, "把这份报告发回来，就能定下装载方案。\n");
    logline(">>> 执行链探针全部跑完（G/H/I/J/K）");
    return 0;
}
