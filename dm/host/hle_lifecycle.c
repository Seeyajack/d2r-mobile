// 宿主生命周期：优雅退出（SIGTERM；iOS 由 App 终止回调调 dm_host_shutdown）。
// 停后台线程（启动块预取）、暂停音频、图形挂起（等在途 NVN 调用结束并清空 GPU 工作），再 _exit(0)；1.5 秒兜底直接 _exit(0)。
// 缺省动作同样约 0.08 秒退出（退出码 143）；本函数的价值是有序停后台线程、退出码 0、给 iOS 生命周期回调一个入口。
// Host lifecycle: graceful exit (SIGTERM; on iOS the App termination callback calls dm_host_shutdown).
// Stop background threads (boot block prefetch), pause audio, suspend graphics (wait for in-flight NVN calls to end and drain GPU work), then _exit(0); a 1.5 s fallback goes straight to _exit(0).
// The default action also exits in about 0.08 s (exit code 143); the value of this function is stopping background threads in order, exit code 0, and giving the iOS lifecycle callback an entry.
#include "hle.h"
#if !defined(__OHOS__)
#include <dispatch/dispatch.h>   // GCD；鸿蒙侧见下面 __OHOS__ 分支（用 pthread 直写）
#endif
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
void tact_prefetch_stop(void);
void granny_prof_dump(void);
void hlecount_flush(void);

#if defined(__OHOS__)
// 鸿蒙没有 GCD，而这里两处都用到了 **block 字面量**（^{...}），所以改用 pthread 直写。
// 为什么不给 dispatch 做个垫片了事：block 要"存下来稍后再调"就必须被复制到堆上，也就是需要一套
// block 运行时；鸿蒙 sysroot 里没有，手搓等于在没真机验证的情况下赌——赌错就是"回调跳进一块已经
// 失效的栈内存"，最难查的那类崩溃。所以不赌，直接写：语义反而更直白，只有两个线程。
// 详见 dm/ohos/compat/dispatch/dispatch.h 的说明。
// HarmonyOS has no GCD, and both spots here use block literals, so they are written with pthread
// directly. The reason is spelled out in compat/dispatch/dispatch.h: a block that must outlive the call
// has to be copied to the heap, which needs a block runtime that this sysroot does not ship, and
// hand-rolling one would be an unverifiable gamble whose failure mode is a callback jumping into dead
// stack memory. Writing it directly is clearer and involves two plain threads.

// 兜底计时：睡满 ms 毫秒后直接退出 —— 对应苹果那一条 dispatch_after。
// Fallback timer: sleep ms milliseconds then exit -- the counterpart of the dispatch_after.
static void *dm_exit_deadline_thread(void *arg) {
    int ms = (int)(intptr_t)arg;
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) && errno == EINTR) { }
    LOG("【红】优雅退出 1.5 秒未完成，直接退出");
    _exit(0);
}
static void dm_exit_deadline_start(int ms) {
    pthread_t t;
    if (!pthread_create(&t, NULL, dm_exit_deadline_thread, (void *)(intptr_t)ms)) pthread_detach(t);
}

// 等 SIGTERM 的线程 —— 对应苹果那一条 GCD 信号源。原注释写的是"交给信号源在普通线程上收尾"，
// sigwait 正好就是这个意思：在普通线程上**同步地**把信号取走，不在信号处理函数里干重活
// （信号处理函数里能安全调用的函数极少，dm_host_shutdown 要停线程、碰图形，绝不能放在那里）。
// The SIGTERM-waiting thread -- the counterpart of the GCD signal source. The original comment says
// "wrap up on an ordinary thread", which is exactly what sigwait does: take the signal synchronously on
// a normal thread instead of doing heavy work inside a handler (very little is safe in a handler;
// dm_host_shutdown stops threads and touches graphics).
static void *dm_sigterm_thread(void *arg) {
    (void)arg;
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGTERM);
    int sig = 0;
    if (sigwait(&set, &sig) == 0) dm_host_shutdown("SIGTERM");
    return NULL;
}
#endif

void dm_host_shutdown(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns(); LOG("收到 %s：优雅退出", why);
#if defined(__OHOS__)
    dm_exit_deadline_start(1500);
#else
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1500 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("【红】优雅退出 1.5 秒未完成，直接退出"); _exit(0); });
#endif
    tact_prefetch_stop();
    granny_prof_dump();
    hlecount_flush();                               // DM_HLECOUNT：_exit 不走 atexit，这里写一份 | DM_HLECOUNT: _exit does not run atexit, write a copy here
    dm_audio_suspend(1);
    dm_gfx_suspend(1);
    LOG("优雅退出完成（%.3f 秒）", (now_ns() - t0) / 1e9);
    _exit(0);
}
void dm_term_install(void) {
#if defined(__OHOS__)
    signal(SIGTERM, SIG_IGN);                       // 不走缺省动作：退出码与收尾交给下面的线程（与苹果同一意图） | do not take the default action: exit code and wrap-up go to the thread below (same intent as Apple)
    // 在本线程**屏蔽** SIGTERM：此后创建的线程都继承这个屏蔽，于是 SIGTERM 一律变成挂起状态、
    // 只会被上面那个 sigwait 取走，不会被别的线程按缺省动作处理掉。
    // Blocking it here means every thread created afterwards inherits the block, so SIGTERM always goes
    // pending and can only be taken by the sigwait above -- never handled by default in another thread.
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t t;
    if (!pthread_create(&t, NULL, dm_sigterm_thread, NULL)) pthread_detach(t);
#else
    signal(SIGTERM, SIG_IGN);                       // 不走缺省动作：交给 GCD 信号源在普通线程上收尾 | do not take the default action: hand over to the GCD signal source to wrap up on an ordinary thread
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
#endif
}
