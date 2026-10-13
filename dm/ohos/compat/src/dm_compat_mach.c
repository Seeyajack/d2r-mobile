// 垫片实现：mach/mach.h 与 mach/mach_time.h 里那些"必须真有实现"的部分。
// Shim implementation: the parts of mach/mach.h and mach/mach_time.h that must really work.
//
// 三类东西分开对待，代码里也分开写：
//   ⓐ 时钟、线程身份      —— 真实现，语义与苹果一致
//   ⓑ 读内存、任务内存统计 —— 真实现，靠 /proc（这是宿主用得最多的 26 处 vm_read_overwrite）
//   ⓒ 别的线程的寄存器/状态 —— 没有等价物，如实返回失败（调用方本来就是"拿不到就少打点"的诊断路径）
// Three groups, kept separate in the code: (a) clock and thread identity -- real, same semantics as
// Apple; (b) memory reads and task memory stats -- real, backed by /proc (this is the 26 calls to
// vm_read_overwrite); (c) other threads' registers/state -- no equivalent exists, so we fail honestly
// (the callers are diagnostic paths that already handle "cannot get it, print less").
#include "mach/mach.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>

// ================================================================ ⓐ 时钟与身份

// 苹果 arm64 上 mach_timebase_info 恒为 1/1，mach_absolute_time 直接就是纳秒。
// 这里给出同样的数值语义，所以上游 `mach_absolute_time() * tb.numer / tb.denom` 不用改、结果不变。
// On Apple arm64 the timebase is always 1/1 and mach_absolute_time is already nanoseconds; the same
// numeric semantics are given here, so upstream's multiply/divide needs no change and yields the same result.
uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

kern_return_t mach_timebase_info(mach_timebase_info_data_t *tb) {
    if (!tb) return KERN_INVALID_ARGUMENT;
    tb->numer = 1;
    tb->denom = 1;
    return KERN_SUCCESS;
}

// 本进程恒为 1；鸿蒙没有 Mach 端口，这个值只用于"是不是本任务"的语义。
// Always 1 for this process; HarmonyOS has no Mach ports, and this value only carries "is this the task".
mach_port_t mach_task_self(void) { return 1; }

// 线程身份就用 pthread_t 的值：这样 hle_thread.c 里 `t->mport == mach_thread_self()`
// （判断"这个线程是不是当前线程"）的语义**与原实现一致**。
// The thread identity is the value of pthread_t, so `t->mport == mach_thread_self()` in hle_thread.c
// ("is this the current thread?") keeps the same meaning as before.
mach_port_t mach_thread_self(void) { return (mach_port_t)(uintptr_t)pthread_self(); }
mach_port_t pthread_mach_thread_np(pthread_t t) { return (mach_port_t)(uintptr_t)t; }

// ================================================================ ⓑ 读内存与内存统计

// 宿主用 vm_read_overwrite 要的是"读一个**可能非法**的地址，读不到就失败、不要崩"。
// 苹果有 Mach 的 vm_read_overwrite，鸿蒙上等价物是 /proc/self/mem 上的 pread：
// 地址非法时 pread 返回 -1 并置 EFAULT，进程不崩。这是两全其美的做法。
// The host wants "read a possibly invalid address, fail instead of crashing". The equivalent is pread on
// /proc/self/mem: a bad address yields -1/EFAULT without killing the process.
//
// 退路是 process_vm_readv(自己)，同样"非法地址返回 -1"。
// 两条都不可用时**返回失败**而不是退回 memcpy —— memcpy 遇到非法地址会直接段错误，
// 那是宿主最不能接受的结果（游戏跑一半崩掉）。宁可少读一段诊断内存。
// The fallback is process_vm_readv(self), which also returns -1 on a bad address. If neither is
// available we fail rather than falling back to memcpy, because memcpy on a bad address segfaults --
// the worst possible outcome for the host. Better to read one less block of diagnostic memory.
static int dm_mem_fd(void) {
    static int fd = -2;
    if (fd == -2) fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    return fd;
}

static int dm_self_read(uint64_t addr, void *buf, size_t n) {
    int fd = dm_mem_fd();
    if (fd >= 0) {
        size_t got = 0;
        int blocked = 0;      // 一个字节都没读到、且错误像是"不让读"（而非"地址非法"）：换下一条路
        while (got < n) {
            ssize_t r = pread(fd, (char *)buf + got, n - got, (off_t)(addr + got));
            if (r > 0) { got += (size_t)r; continue; }
            if (r < 0 && errno == EINTR) continue;
            if (!got && (errno == EPERM || errno == EACCES || errno == ENOSYS))
                blocked = 1;                       // 被 seccomp 之类拦下：试 process_vm_readv
            break;                                 // EFAULT = 地址真的非法：直接失败，这是正常结果
        }
        if (!blocked) return got == n ? 0 : -1;
    }
#ifdef SYS_process_vm_readv
    {
        struct iovec local  = { buf, n };
        struct iovec remote = { (void *)(uintptr_t)addr, n };
        long r = syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0);
        return r == (long)n ? 0 : -1;
    }
#else
    return -1;
#endif
}

kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size,
                                vm_address_t data, vm_size_t *outsize) {
    (void)task;
    if (outsize) *outsize = 0;
    if (!size) return KERN_SUCCESS;
    if (dm_self_read(addr, (void *)(uintptr_t)data, (size_t)size)) return KERN_FAILURE;
    if (outsize) *outsize = size;
    return KERN_SUCCESS;
}

kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size) {
    (void)task;
    // 宿主只在 task_threads 成功之后对返回的数组调它；鸿蒙侧 task_threads 恒失败，
    // 所以这条路径目前不可达。给一个 munmap 实现，防止将来有人改成别的用法而漏内存。
    // Unreachable today (task_threads always fails); a munmap implementation guards against future misuse.
    if (munmap((void *)(uintptr_t)addr, (size_t)size) && errno != EINVAL) return KERN_FAILURE;
    return KERN_SUCCESS;
}

// 任务内存统计：phys_footprint 用 /proc/self/statm 的 RSS（常驻内存）近似。
// 苹果的 phys_footprint 含压缩内存，与 RSS 口径不完全相同；这里用于"内存走势观测"，
// 绝对值不参与任何判断，趋势一致就够用。口径差异在 README 里注明。
// phys_footprint is approximated by RSS from /proc/self/statm. Apple's phys_footprint also counts
// compressed memory, so the absolute value differs; it is used only for observing the memory trend.
kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *count) {
    (void)task;
    if (!info || !count) return KERN_INVALID_ARGUMENT;
    if (flavor != TASK_VM_INFO || *count < TASK_VM_INFO_COUNT) return KERN_INVALID_ARGUMENT;
    task_vm_info_data_t *vi = (task_vm_info_data_t *)info;
    memset(vi, 0, sizeof *vi);

    long resident = 0;
    FILE *fp = fopen("/proc/self/statm", "r");
    if (fp) {
        long size = 0, shared = 0, text = 0, lib = 0, data = 0, dt = 0;
        if (fscanf(fp, "%ld %ld %ld %ld %ld %ld %ld", &size, &resident, &shared, &text, &lib, &data, &dt) < 2)
            resident = 0;
        fclose(fp);
    }
    long pgsz = sysconf(_SC_PAGESIZE);
    vi->count = TASK_VM_INFO_COUNT;
    vi->phys_footprint = (uint64_t)resident * (uint64_t)(pgsz > 0 ? pgsz : 4096);
    *count = TASK_VM_INFO_COUNT;
    return KERN_SUCCESS;
}

// ================================================================ ⓒ 别的线程的状态：如实失败

// 宿主里用到这些的只有一处：hle_thread.c 的 thr_bt()（看门狗 / 崩溃时导出**别的客体线程**
// 的寄存器和栈，用来事后分析"当时卡在哪"）。
// Linux/鸿蒙没有"非特权读另一个线程寄存器"的接口：ptrace 要权限、要挂起目标，而且会改变目标状态，
// 在游戏进程里对自己线程做这件事风险和收益完全不成比例。
// 所以这里一律返回 KERN_FAILURE ⇒ thr_bt() 在第一步 thread_suspend 就返回，
// 看门狗日志里少一段"别的线程的寄存器/栈"，**游戏行为一行不受影响**。
// The only user is thr_bt() in hle_thread.c (exporting other guest threads' registers and stacks on a
// watchdog/crash). Linux/OHOS has no unprivileged way to read another thread's registers, and ptrace --
// which needs privileges and mutates the target -- is wildly disproportionate here. Failing at the first
// step (thread_suspend) shortens the watchdog log and changes no game behaviour.
static void dm_thread_api_warn(const char *what) {
    static const char *last;
    if (last == what) return;
    last = what;
    fprintf(stderr, "[dm] 【说明】%s：鸿蒙上无等价接口，看门狗现场导出会少一段（游戏行为不受影响）\n", what);
}

kern_return_t thread_suspend(mach_port_t t) { (void)t; dm_thread_api_warn("thread_suspend"); return KERN_FAILURE; }
kern_return_t thread_resume(mach_port_t t) { (void)t; return KERN_FAILURE; }
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t st, mach_msg_type_number_t *n) {
    (void)t; (void)flavor; (void)st; (void)n;
    dm_thread_api_warn("thread_get_state");
    return KERN_FAILURE;
}
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t info, mach_msg_type_number_t *n) {
    (void)t; (void)flavor; (void)info; (void)n;
    dm_thread_api_warn("thread_info");
    return KERN_FAILURE;
}
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *n) {
    (void)task; (void)list; (void)n;
    dm_thread_api_warn("task_threads");
    return KERN_FAILURE;
}
