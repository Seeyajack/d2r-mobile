// dispatch/dispatch.h 的实现：目前只有信号量（hle_audio.c 需要）。
// Implementation of dispatch/dispatch.h: only semaphores so far (needed by hle_audio.c).
//
// 语义对齐 GCD 的两点，这两点是调用方依赖的：
//   ① dispatch_semaphore_wait 成功返回 0、超时返回非 0；
//   ② dispatch_semaphore_signal 有线程被唤醒时返回非 0。
// Two GCD semantics the callers rely on: wait returns 0 on success and non-zero on timeout; signal
// returns non-zero when a thread was woken.
#include "dispatch/dispatch.h"

#include <errno.h>
#include <stdlib.h>
#include <time.h>

struct dm_dispatch_semaphore_s {
    pthread_mutex_t m;
    pthread_cond_t  c;
    long v;          // 计数 | count
    long waiters;    // 正在等的线程数（signal 靠它决定返回 0 还是非 0） | threads waiting (decides signal's return)
    long refs;       // dispatch_retain / dispatch_release 的引用计数 | reference count
};

// 鸿蒙侧没有 GCD 队列：宿主此处的用法只是"随便给个队列"当占位（真正排队的是别的东西），
// 所以给一个稳定的哑对象，保证指针非空、可比较。
// No GCD queues on HarmonyOS: the host only uses the queue as a placeholder, so return a stable dummy
// object that is non-NULL and comparable.
struct dm_dispatch_queue_s { int dummy; };
static struct dm_dispatch_queue_s g_global_queue;

static uint64_t dm_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * NSEC_PER_SEC + (uint64_t)ts.tv_nsec;
}

dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return DISPATCH_TIME_FOREVER;
    uint64_t base = (when == DISPATCH_TIME_NOW) ? dm_now_ns() : when;
    if (delta < 0) {
        uint64_t d = (uint64_t)(-delta);
        return base > d ? base - d : 0;
    }
    return base + (uint64_t)delta;
}

dispatch_queue_t dispatch_get_global_queue(long identifier, unsigned long flags) {
    (void)identifier; (void)flags;
    return &g_global_queue;
}

dispatch_semaphore_t dispatch_semaphore_create(long value) {
    if (value < 0) return NULL;                      // GCD 对负值的行为未定义；这里明确拒绝
    struct dm_dispatch_semaphore_s *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->c, NULL);
    s->v = value;
    s->refs = 1;
    return s;
}

long dispatch_semaphore_signal(dispatch_semaphore_t s) {
    if (!s) return 0;
    pthread_mutex_lock(&s->m);
    long woken = s->waiters > 0;                     // 有线程在等 → 返回非 0（与 GCD 一致）
    s->v++;
    pthread_cond_signal(&s->c);
    pthread_mutex_unlock(&s->m);
    return woken;
}

long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout) {
    if (!s) return -1;
    pthread_mutex_lock(&s->m);
    for (;;) {
        if (s->v > 0) { s->v--; pthread_mutex_unlock(&s->m); return 0; }
        if (timeout == DISPATCH_TIME_FOREVER) {
            s->waiters++; pthread_cond_wait(&s->c, &s->m); s->waiters--;
            continue;
        }
        if (dm_now_ns() >= timeout) { pthread_mutex_unlock(&s->m); return 1; }
        struct timespec ts = { .tv_sec = (time_t)(timeout / NSEC_PER_SEC),
                               .tv_nsec = (long)(timeout % NSEC_PER_SEC) };
        s->waiters++;
        int r = pthread_cond_timedwait(&s->c, &s->m, &ts);
        s->waiters--;
        if (r == ETIMEDOUT && s->v <= 0) { pthread_mutex_unlock(&s->m); return 1; }
    }
}

void dispatch_retain(void *object) {
    struct dm_dispatch_semaphore_s *s = object;
    if (!s) return;
    pthread_mutex_lock(&s->m);
    s->refs++;
    pthread_mutex_unlock(&s->m);
}

void dispatch_release(void *object) {
    struct dm_dispatch_semaphore_s *s = object;
    if (!s) return;
    pthread_mutex_lock(&s->m);
    long r = --s->refs;
    pthread_mutex_unlock(&s->m);
    if (r <= 0) {
        pthread_mutex_destroy(&s->m);
        pthread_cond_destroy(&s->c);
        free(s);
    }
}
