// 垫片：苹果 <mach/mach.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <mach/mach.h>.
//
// 这是整份垫片里最需要说清楚的一块，因为宿主对 Mach 的用法分成三种性质完全不同的东西：
// This is the part of the shim that needs the most explanation, because the host uses Mach for three
// fundamentally different things:
//
//   ① 取时钟      mach_absolute_time —— 有干净的 POSIX 等价物（见 mach/mach_time.h）。真实实现。
//      Clock      mach_absolute_time -- has a clean POSIX equivalent (see mach/mach_time.h). Real implementation.
//
//   ② 安全读内存  vm_read_overwrite(mach_task_self(), addr, n, buf, &got) —— 宿主用了 26 处，
//                 分布在 diag.c / dm_bright.c / hle_target.c / hle_private_aim.c / hle_thread.c。
//                 它要的是"读一个可能非法的地址，读不到就返回失败、不要崩"。
//                 鸿蒙上的等价物是 /proc/self/mem + pread：地址非法时 pread 返回 -1，进程不崩。
//      Safe reads  vm_read_overwrite used 26 times; it wants "read a possibly invalid address, fail instead of
//                 crashing". The equivalent is /proc/self/mem + pread: a bad address gives -1, no crash.
//
//   ③ 线程与任务  thread_get_state / thread_suspend / task_threads / task_info —— 这段**没有**等价物。
//                 见下面 "未实现" 一节：我们只做诚实的降级，不做假装成功的桩。
//      Threads     thread_get_state / thread_suspend / task_threads / task_info -- no equivalent exists; see
//                 the "not implemented" section below. We degrade honestly rather than fake success.
#pragma once

#include "dm_compat_types.h"
#include "mach/mach_time.h"
#include <pthread.h>

// ---------- ① 时钟（已包含 mach_time.h） ----------

// ---------- ② 内存读取 ----------
// 真实实现：src/dm_compat_mach.c 用 /proc/self/mem + pread。
// 返回 KERN_SUCCESS 表示读满 size 字节，把实际读到的长度写进 *outsize；失败返回 KERN_FAILURE。
// Real implementation in src/dm_compat_mach.c via /proc/self/mem + pread.
// KERN_SUCCESS means `size` bytes were read; the actual count goes to *outsize; failure gives KERN_FAILURE.
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size,
                                vm_address_t data, vm_size_t *outsize);

// vm_deallocate：鸿蒙侧没有 Mach 里的分发端口，内存由 mmap/munmap 管；
// 宿主只在 hle_thread.c 里对 task_threads 返回的数组调它。提供真实实现（munmap 语义）以免漏内存。
// Only used in hle_thread.c on the array returned by task_threads; a real munmap-backed implementation.
kern_return_t vm_deallocate(mach_port_t task, vm_address_t addr, vm_size_t size);

// mach_task_self：恒为 1（本进程）。真实实现，用来对齐 vm_read_overwrite / task_info 的入参语义。
#define MACH_PORT_NULL 0
mach_port_t mach_task_self(void);

// ---------- ③ 线程身份（真实实现） ----------
// mach_thread_self() 与 pthread_mach_thread_np(t) 都返回"线程身份"（就是 pthread_t 的值）。
// 这样 hle_thread.c 里 `t->mport == mach_thread_self()`（判断是不是当前线程）的语义保持不变。
// Both return the "thread identity" (the value of pthread_t), so `t->mport == mach_thread_self()`
// in hle_thread.c (is this the current thread?) keeps its meaning.
mach_port_t mach_thread_self(void);
mach_port_t pthread_mach_thread_np(pthread_t t);

// ---------- ③ 线程状态：结构按 Darwin 布局给足，好让上游源码原样编译 ----------
// DSM: 这些结构只为了"类型对得上"。真正的读取在下面一律返回失败，见"未实现"。
// These types exist only so that the upstream source compiles as is; the actual reads below always fail.

typedef struct arm_thread_state64_s {
    uint64_t __x[29];
    uint64_t __fp;
    uint64_t __lr;
    uint64_t __sp;
    uint64_t __pc;
    uint32_t __cpsr;
    uint32_t __pad;
} arm_thread_state64_t;

typedef uint32_t *thread_state_t;
typedef integer_t *thread_info_t;
typedef mach_port_t *thread_act_array_t;

#define ARM_THREAD_STATE64        6
#define ARM_THREAD_STATE64_COUNT  ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / sizeof(uint32_t)))

// 上游用这几个取寄存器字段；Darwin 上是内联函数，鸿蒙上直接映射到结构成员。
#define __darwin_arm_thread_state64_get_pc(st) ((st).__pc)
#define __darwin_arm_thread_state64_get_fp(st) ((st).__fp)
#define __darwin_arm_thread_state64_get_lr(st) ((st).__lr)
#define __darwin_arm_thread_state64_get_sp(st) ((st).__sp)

typedef struct thread_basic_info_s {
    struct { int32_t seconds; int32_t microseconds; } user_time, system_time;
    int32_t cpu_usage;
    int32_t policy;
    int32_t run_state;
    int32_t flags;
    int32_t suspend_count;
    int32_t sleep_time;
} thread_basic_info_data_t;

#define THREAD_BASIC_INFO        3
#define THREAD_BASIC_INFO_COUNT  ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / sizeof(uint32_t)))

// 任务内存统计：上游只读 phys_footprint（diag.c 两处）。真实实现从 /proc/self/statm 算 RSS。
// Task memory stats: upstream reads only phys_footprint (two places in diag.c). The real implementation
// derives it from /proc/self/statm (RSS).
typedef struct task_vm_info_s {
    mach_msg_type_number_t count;
    uint64_t phys_footprint;
} task_vm_info_data_t;

#define TASK_VM_INFO             22
#define TASK_VM_INFO_COUNT       ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(uint32_t)))

kern_return_t task_info(mach_port_t task, int flavor, task_info_t info, mach_msg_type_number_t *count);

// ---------- ③ 未实现：一律返回 KERN_FAILURE，让调用方走它自己的"拿不到就跳过"分支 ----------
// 依据（不是偷懒，是核实过的）：这些调用在宿主里只出现在"出问题时打印现场"的诊断路径
//   hle_thread.c:308 thr_bt()  —— 看门狗 / 崩溃时导出别的客体线程的寄存器和栈
// Linux/鸿蒙 没有"读另一个线程寄存器"的非特权接口（ptrace 需要权限且会改变目标线程状态）。
// 所以这里返回失败 ⇒ thr_bt() 在 thread_suspend 那一步就 return，少打一屏诊断信息，
// **游戏逻辑一行不受影响**。宁可少打日志，也不假装成功、打出错的数据。
// These calls appear only in the "dump the scene when something goes wrong" diagnostic path
// (hle_thread.c thr_bt()). Linux/OHOS has no unprivileged API to read another thread's registers.
// Failing here means thr_bt() returns at its thread_suspend step and prints less; game logic is unaffected.
// Better less logging than fake success with wrong data.
kern_return_t thread_suspend(mach_port_t t);
kern_return_t thread_resume(mach_port_t t);
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t st, mach_msg_type_number_t *n);
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t info, mach_msg_type_number_t *n);
kern_return_t task_threads(mach_port_t task, thread_act_array_t *list, mach_msg_type_number_t *n);
