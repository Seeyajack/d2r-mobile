// 垫片公共类型：Mach 内核接口用到的那几个基础类型，集中在这里定义一次。
// Shared shim types: the few fundamental types used by the Mach kernel interfaces, defined once here.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef int kern_return_t;
typedef int vm_prot_t;
typedef int integer_t;
typedef int32_t *task_info_t;

// 注意：这里的 mach_port_t 不是苹果的 32 位端口，而是"线程/任务身份标记"。
// 鸿蒙没有 Mach 端口，我们用 pthread_t 的值当身份：mach_thread_self() 与 pthread_mach_thread_np()
// 返回同一个值，hle_thread.c 里 `t->mport == mach_thread_self()`（判断"是不是当前线程"）因此仍然成立。
// Note: mach_port_t here is not Apple's 32-bit port but a "thread/task identity token". OHOS has no Mach ports,
// so the value of pthread_t is used as the identity: mach_thread_self() and pthread_mach_thread_np() return the
// same value, so `t->mport == mach_thread_self()` in hle_thread.c ("is this the current thread?") still holds.
typedef uintptr_t mach_port_t;
typedef uint64_t vm_address_t;
typedef uint64_t vm_size_t;
typedef uint32_t mach_msg_type_number_t;

#define KERN_SUCCESS           0
#define KERN_INVALID_ARGUMENT  4
#define KERN_FAILURE           5
