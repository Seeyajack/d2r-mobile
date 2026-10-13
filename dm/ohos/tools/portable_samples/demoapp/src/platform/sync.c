// 同步：GCD 信号量 + 苹果的线程服务质量（qos）+ 苹果的栈信息接口。
// Synchronisation: GCD semaphores, Apple's thread QoS, and Apple's stack introspection.
//
// 注意这里**不含 block**：block 那块垫片层刻意没做（见 compat/dispatch/dispatch.h 的说明），
// 所以样本也不放——放进来会得到一个"已知的、有意的"失败，污染报告。
// Note: no blocks here. The shim layer deliberately does not provide them (see
// compat/dispatch/dispatch.h), so this sample omits them too; including one would produce a known,
// intentional failure and pollute the report.
#include "demoapp.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdlib.h>

struct demo_latch { dispatch_semaphore_t sem; };

demo_latch *demo_latch_new(void) {
    demo_latch *l = calloc(1, sizeof *l);
    if (l) l->sem = dispatch_semaphore_create(0);
    return l;
}

void demo_latch_signal(demo_latch *l) { if (l) dispatch_semaphore_signal(l->sem); }

int demo_latch_wait(demo_latch *l) {
    return l ? (int)dispatch_semaphore_wait(l->sem, DISPATCH_TIME_FOREVER) : -1;
}

int demo_latch_wait_ms(demo_latch *l, int ms) {
    if (!l) return -1;
    return (int)dispatch_semaphore_wait(l->sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC));
}

void demo_latch_free(demo_latch *l) {
    if (!l) return;
    dispatch_release(l->sem);
    free(l);
}

void demo_boost(void) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

// 苹果的栈信息：返回的"栈地址"是**栈顶**（高地址）。
// Apple's stack info: the returned "stack address" is the stack base (the high address).
int demo_stack_top(void *p) {
    void *top = pthread_get_stackaddr_np(pthread_self());
    size_t sz = pthread_get_stacksize_np(pthread_self());
    if (!top || !sz) return 0;
    return (p < top && (char *)p >= (char *)top - sz) ? 1 : 0;
}
