// Horizon svc 同步原语（L1 通用）：事件 / 锁 / 条件变量 / 同步对象等待。
// Horizon svc synchronisation primitives (L1, generic): events, arbitrated locks,
// process-wide keys (condition variables) and multi-object waits.
//
// 为什么要这层（2026-10-20，M1 的前置）：
//   我们目前把 `nn::os` 的锁/事件/信号量**在导入层**用 pthread 实现（过渡件，能用）。
//   但 Switch 的 **SDK 模块自带 nn::os**，它不这么做——它站在 svc 的同步原语上：
//     0x1A/0x1B 地址仲裁锁、0x1C/0x1D 进程级 key（条件变量）、0x18 多对象等待、0x11/0x12/0x17 事件。
//   不补这层就往 sdk 切，第一把锁就会挂。
// Why this layer: we currently implement nn::os on top of pthreads at the import level (a stopgap).
// The Switch SDK ships its own nn::os, which instead sits on these svc primitives; switching to it
// without this layer would hang on the first lock.
//
// 语义来源（必读，许可相关）：
//   - 语义整理自 seedds/nxnative（MIT）的 dm/host/hle_thread.c，并用 Atmosphère（Horizon 内核公开实现）交叉印证。
//     本文件是**按规范重写**，不是拷贝；若日后直接移植其代码，须在 NOTICE.md 逐项登记。
//   - 规范见同目录 `svc同步层-规范与移植计划.md`。
// Semantics are documented in svc同步层-规范与移植计划.md and were derived from seedds/nxnative (MIT)
// and Atmosphère, then re-implemented here. This file is a rewrite, not a copy.
//
// ⚠ 最重要的一条时序（写错就是永久等待，且不报错）：
//   WaitProcessWideKeyAtomic 必须 **先置 key → 再放锁 → 再入队**。
//   反序会让"放锁之后、置 key 之前"改完条件的线程读到 key=0 不唤醒 ⇒ 本线程永远睡着。
#include "dm_svc_sync.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

static void (*g_log)(const char *, ...);
void dm_svc_sync_set_log(void (*fn)(const char *, ...)) { g_log = fn; }
#define SLOG(...) do { if (g_log) g_log(__VA_ARGS__); } while (0)

// ── 句柄表（对象类型 + 事件置位标志）────────────────────────────────────
// 句柄值 = 0x1000 + 下标；type 用于校验（SignalEvent 只认写端，ResetSignal 只认读端）。
enum { O_NONE = 0, O_EVENT = 1, O_THREAD = 2 };
#define SYNC_H_MAX 4096
static struct { int type; int signaled; } g_h[SYNC_H_MAX];

static int h_index(uint32_t h) {
    if (h < 0x1000 || h - 0x1000 >= SYNC_H_MAX) return -1;
    int i = (int)(h - 0x1000);
    return g_h[i].type == O_NONE ? -1 : i;
}

// ── 等待链 ────────────────────────────────────────────────────────────
// 每线程一个 waiter（放 TLS 最省事，也天然满足"一线程同时只有一个等待"的内核约定）。
// kind: 1=仲裁锁 2=进程级 key 4=多对象同步
typedef struct waiter {
    int      kind;
    uint64_t addr, key;
    uint32_t tag;
    uint64_t seq;            // 注册序号：同优先级 FIFO（内核语义）
    int64_t  res;            // 唤醒结果（写进 regs[0]）
    int      cancel;
    int      hit;            // 多对象等待：触发的下标
    const uint32_t *handles; // 多对象等待：句柄数组（指向客体内存）
    int      nh;
    pthread_cond_t cv;       // **每线程一个**条件变量（同一个 waiter 只会被一个线程用）
    struct waiter *next;
} waiter_t;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static waiter_t *g_wait;
static uint64_t g_seq;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void waiter_once(void) {
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_condattr_destroy(&ca);
}
static __thread waiter_t t_w;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// 入链 → 等（可带超时）→ 出链。返回 res（0=被正常唤醒；-1=超时；-2=取消）。
// 调用时**不持 G**。
static int64_t wait_block(waiter_t *w, int kind, uint64_t addr, uint64_t key,
                          uint32_t tag, int64_t timeout_ns) {
    pthread_once(&g_once, waiter_once);
    pthread_mutex_lock(&G);
    w->kind = kind; w->addr = addr; w->key = key; w->tag = tag;
    w->res = 1; w->cancel = 0; w->hit = -1;
    w->seq = ++g_seq;
    w->next = g_wait; g_wait = w;
    int64_t deadline = timeout_ns < 0 ? -1 : now_ns() + timeout_ns;
    while (w->res == 1) {                     // 1 = 还没被唤醒
        if (deadline < 0) {
            pthread_cond_wait(&t_w.cv, &G);
        } else {
            struct timespec ts = { deadline / 1000000000, deadline % 1000000000 };
            if (pthread_cond_timedwait(&t_w.cv, &G, &ts) != 0 && w->res == 1) {
                w->res = -1;                  // 超时
                break;
            }
        }
    }
    // 出链
    waiter_t **pp = &g_wait;
    while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
    w->next = NULL;
    int64_t r = w->res;
    pthread_mutex_unlock(&G);
    return r;
}

// 在持有 G 的前提下唤醒一个 waiter（把它移出链并 signal）
static void wake_locked(waiter_t *w, int64_t res) {
    waiter_t **pp = &g_wait;
    while (*pp) { if (*pp == w) { *pp = w->next; break; } pp = &(*pp)->next; }
    w->next = NULL;
    w->res = res;
    pthread_cond_signal(&w->cv);
}

// 按内核顺序挑一个：优先级（这里一律 0，同优先级）按 seq 先注册先选
static waiter_t *pick_locked(int kind, uint64_t addr, uint64_t key, int match_key) {
    waiter_t *best = NULL;
    for (waiter_t *w = g_wait; w; w = w->next) {
        if (w->kind != kind) continue;
        if (match_key ? (w->key != key) : (w->addr != addr)) continue;
        if (!best || w->seq < best->seq) best = w;
    }
    return best;
}

static int has_more_locked(int kind, uint64_t addr) {
    for (waiter_t *w = g_wait; w; w = w->next)
        if (w->kind == kind && w->addr == addr) return 1;
    return 0;
}

// ── 锁字：owner_handle（低 30 位） | 有等待者 0x40000000 ────────────────
#define WAITERS_BIT 0x40000000u
static inline uint32_t ld32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline void st32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }

// 解锁：无等待者清零；有等待者**直接把锁交给队首**（handoff，不是唤醒后抢）
static void unlock_handoff_locked(uint64_t addr) {
    pthread_mutex_lock(&G);
    waiter_t *w = pick_locked(1, addr, 0, 0);
    if (!w) {
        st32(addr, 0);
    } else {
        int more = 0;
        for (waiter_t *x = g_wait; x; x = x->next)
            if (x != w && x->kind == 1 && x->addr == addr) { more = 1; break; }
        st32(addr, w->tag | (more ? WAITERS_BIT : 0));
        wake_locked(w, 0);
    }
    pthread_mutex_unlock(&G);
}

// 条件变量唤醒时给某个 waiter 重新拿锁：空闲就给它，被占就把它转成锁等待
static void lock_acquire_for_locked(waiter_t *w) {
    uint32_t cur = ld32(w->addr);
    if (cur == 0) {
        st32(w->addr, w->tag);
        wake_locked(w, 0);
    } else {
        st32(w->addr, cur | WAITERS_BIT);
        w->kind = 1;                 // 转为锁等待
        w->seq = ++g_seq;            // 重新编号（内核语义）
    }
}

void dm_svc_sync_init(void) {
    pthread_once(&g_once, waiter_once);
    static __thread int done;
    if (!done) { done = 1; pthread_cond_init(&t_w.cv, NULL); }
}

// ── svc 分派 ──────────────────────────────────────────────────────────
// 返回 1 = 已处理（*ret 为 regs[0] 的返回值），0 = 不归本层管。
int dm_svc_sync_dispatch(int num, uint64_t *regs, uint64_t *ret) {
    switch (num) {
    case 0x11: {                       // SignalEvent(handle)：只认写端
        int i = h_index((uint32_t)regs[0]);
        if (i < 0) { *ret = 0xE401; return 1; }
        pthread_mutex_lock(&G);
        g_h[i].signaled = 1;
        for (waiter_t *w = g_wait; w; w = w->next) {
            if (w->kind != 4) continue;
            for (int k = 0; k < w->nh; k++)
                if (w->handles[k] == regs[0]) { wake_locked(w, 0); break; }
        }
        pthread_mutex_unlock(&G);
        *ret = 0; return 1;
    }
    case 0x12: {                       // ClearEvent(handle)：读写端都接受
        int i = h_index((uint32_t)regs[0]);
        if (i < 0) { *ret = 0xE401; return 1; }
        g_h[i].signaled = 0;
        *ret = 0; return 1;
    }
    case 0x17: {                       // ResetSignal(handle)：只认读端
        int i = h_index((uint32_t)regs[0]);
        if (i < 0) { *ret = 0xE401; return 1; }
        if (g_h[i].type == O_THREAD || !g_h[i].signaled) { *ret = 0xFA01; return 1; }
        g_h[i].signaled = 0;
        *ret = 0; return 1;
    }
    case 0x18: {                       // WaitSynchronization(handles, n, timeout) → 触发下标在 x1
        const uint32_t *hs = (const uint32_t *)(uintptr_t)regs[0];
        int n = (int)regs[1];
        int64_t to = (int64_t)regs[2];
        if (!hs || n <= 0 || n > 64) { *ret = 0xE401; return 1; }
        for (;;) {
            for (int k = 0; k < n; k++) {          // 先扫一遍：有已触发的就直接返回
                int i = h_index(hs[k]);
                if (i < 0) { *ret = 0xE401; return 1; }
                if (g_h[i].signaled) { g_h[i].signaled = 0; regs[1] = (uint64_t)k; *ret = 0; return 1; }
            }
            waiter_t *w = &t_w;
            w->handles = hs; w->nh = n;
            int64_t r = wait_block(w, 4, 0, 0, 0, to);
            if (r == -1) { *ret = 0xEA01; return 1; }
            if (w->cancel) { *ret = 0xEC01; return 1; }
            // 被唤醒后重扫（循环）
        }
    }
    case 0x19: {                       // CancelSynchronization(thread_handle)
        pthread_mutex_lock(&G);
        for (waiter_t *w = g_wait; w; w = w->next)
            if (w->kind == 4) { w->cancel = 1; wake_locked(w, 0); }
        pthread_mutex_unlock(&G);
        *ret = 0; return 1;
    }
    case 0x1A: {                       // ArbitrateLock(addr, owner_handle, tag)
        uint64_t addr = regs[0]; uint32_t tag = (uint32_t)regs[2];
        if (!addr) { *ret = 0; return 1; }
        if (ld32(addr) == (tag | WAITERS_BIT)) { *ret = 0; return 1; }   // 已把锁交给我
        st32(addr, tag | WAITERS_BIT);                                    // 声明"我在等"
        waiter_t *w = &t_w;
        wait_block(w, 1, addr, 0, tag, -1);
        *ret = 0; return 1;
    }
    case 0x1B: {                       // ArbitrateUnlock(addr)
        if (regs[0]) unlock_handoff_locked(regs[0]);
        *ret = 0; return 1;
    }
    case 0x1C: {                       // WaitProcessWideKeyAtomic(addr, key, tag, timeout)
        uint64_t addr = regs[0], key = regs[1];
        uint32_t tag = (uint32_t)regs[2];
        int64_t to = (int64_t)regs[3];
        if (!addr || !key) { *ret = 0; return 1; }
        st32(key, 1);                       // ① 先置 key（顺序不能换！见文件头）
        unlock_handoff_locked(addr);        // ② 再放锁
        waiter_t *w = &t_w;
        int64_t r = wait_block(w, 2, addr, key, tag, to);   // ③ 再入队
        (void)r;                            // 超时后**不重新拿锁**（用户态自己拿）
        *ret = 0; return 1;
    }
    case 0x1D: {                       // SignalProcessWideKey(key, count)
        uint64_t key = regs[0];
        int32_t cnt = (int32_t)regs[1];
        if (!key) { *ret = 0; return 1; }
        pthread_mutex_lock(&G);
        int done = 0;
        for (;;) {
            if (cnt > 0 && done >= cnt) break;
            waiter_t *w = pick_locked(2, 0, key, 1);      // 按 key 匹配
            if (!w) break;
            lock_acquire_for_locked(w);
            done++;
        }
        if (!pick_locked(2, 0, key, 1)) st32(key, 0);     // 没等待者了 → 清零
        pthread_mutex_unlock(&G);
        *ret = 0; return 1;
    }
    default:
        return 0;
    }
}
