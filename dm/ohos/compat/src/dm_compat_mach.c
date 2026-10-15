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

// 苹果独有的纳秒时钟。**必须有真实现、且返回 64 位**——理由见 compat/shim_prefix.h：
// 不声明它编译器只会给警告，然后当成返回 int，纳秒值被静默截断到 32 位。
// 宿主有 4 个文件在用它。
// Apple's nanosecond clock. It must really exist and return 64 bits -- see compat/shim_prefix.h:
// without a declaration the compiler only warns, then assumes int and silently truncates the value to
// 32 bits. Four host files call it.
uint64_t clock_gettime_nsec_np(clockid_t clock_id) {
    struct timespec ts;
    if (clock_gettime(clock_id, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// 栈信息：苹果返回**栈顶**（高地址），而 Linux 的 pthread_attr_getstack 给的是**栈底**（低地址），
// 所以这里换算成苹果的口径。hle_svc.c 用 top 与 top−size 框出 [栈底, 栈顶) 来判断
// "某个地址是不是落在当前线程的栈上"——口径错了它就会把栈上的地址当成代码页。
// Stack info: Apple returns the stack base (high address) while Linux's pthread_attr_getstack yields the
// low address, so the value is converted to Apple's convention. hle_svc.c frames [top-size, top) to decide
// whether an address lies on the current thread's stack; the wrong convention would misclassify stack
// addresses as code pages.
static int dm_stack_of(pthread_t t, void **base, size_t *size) {
    pthread_attr_t attr;
    *base = NULL; *size = 0;
    if (pthread_getattr_np(t, &attr)) return -1;
    int rc = pthread_attr_getstack(&attr, base, size);
    pthread_attr_destroy(&attr);
    return rc ? -1 : 0;
}
void *pthread_get_stackaddr_np(pthread_t t) {
    void *base = NULL; size_t size = 0;
    if (dm_stack_of(t, &base, &size) || !base) return NULL;   // 取不到就返回 NULL，调用方有对应分支
    return (char *)base + size;                                // 低地址 + 大小 = 栈顶（苹果口径）
}
size_t pthread_get_stacksize_np(pthread_t t) {
    void *base = NULL; size_t size = 0;
    if (dm_stack_of(t, &base, &size)) return 0;
    return size;
}

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

// 与 vm_deallocate 同理：唯一调用点在 task_threads 成功之后，而 task_threads 恒失败 ⇒ 不可达。
// 返回成功是如实的——**确实没有任何资源被分配过**，不是假装成功。
// Same as vm_deallocate: the only call site sits after a successful task_threads, which never happens
// here, so it is unreachable. Returning success is truthful -- nothing was ever allocated.
kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) {
    (void)task; (void)name;
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

// ---- libSystem 私有接口 thread_selfcounts：本线程累计 {指令数, 周期数}（并入上游时新带进来的依赖）----
// 上游用它做"每帧每个模块执行了多少条指令"的读数（`hle_nvn.c` 的 thr_instr / thr_counts、`dm_prof.c`）。
// 鸿蒙一侧**没有指令计数器**：没有等价的私有接口，/proc 也只给 CPU 时间、不给指令数。
// 所以这里**如实返回失败**并把调用方的缓冲清零 —— 调用方读到 0 就知道"这项读数不可用"，
// 而不是读到垃圾数（它们本来就是 DM_PUMP_PROF 这类诊断开关，读数是 0 就是没生效，不影响游戏行为）。
// 想要同类信息，正路是用 clock_gettime(CLOCK_THREAD_CPUTIME_ID) 量时间 —— 那是性能调优阶段的事。
int thread_selfcounts(int type, void *buf, size_t nbytes) {
    (void)type;
    if (buf && nbytes) memset(buf, 0, nbytes);
    return -1;
}
