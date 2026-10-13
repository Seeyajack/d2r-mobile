// 内存：Mach 的安全读内存、任务内存统计、苹果的 malloc 统计、sysctl 取进程启动时刻。
// Memory: Mach safe reads, task memory stats, Apple malloc statistics, and sysctl for process start time.
#include "demoapp.h"

#include <mach/mach.h>
#include <malloc/malloc.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

// 安全读：地址非法时**失败而不是崩**——这是 Mach 那套接口在宿主里的核心用途（26 处）
int demo_peek(uint64_t addr, void *out, size_t n) {
    vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), (vm_address_t)addr, (vm_size_t)n,
                          (vm_address_t)out, &got) != KERN_SUCCESS) return 0;
    return got == n;
}

uint64_t demo_footprint(void) {
    task_vm_info_data_t vi;
    mach_msg_type_number_t c = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &c) != KERN_SUCCESS) return 0;
    return vi.phys_footprint;
}

uint64_t demo_malloc_bytes(void) {
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    return st.size_in_use;
}

int demo_start_time(struct timeval *tv) {
    struct kinfo_proc kp;
    size_t n = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return -1;
    *tv = kp.kp_proc.p_starttime;
    return 0;
}
