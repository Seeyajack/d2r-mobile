// 垫片：苹果 <pthread/qos.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <pthread/qos.h>.
//
// 宿主用它干嘛：给线程标一个"服务质量等级"（用户交互 / 用户发起 / 实用 / 未指定），
// 苹果的调度器据此决定线程的优先级与能效取向。这**纯粹是给调度器的建议**，
// 不影响任何功能正确性——去掉它线程照跑，只是优先级听系统默认的。
// The host marks a thread with a "quality of service" class; Apple's scheduler uses it as a hint for
// priority and energy. It is purely a scheduler hint and affects no functionality.
//
// 鸿蒙侧处理：保留类型与函数名让上游源码原样编译，调用一律返回成功但不做事
// （不假装改了优先级，因为确实没改）。将来若要真调优先级，正确做法是
// pthread_setschedparam / setpriority，那属于性能调优阶段的事，不在宿主编译这条线上。
// HarmonyOS side: keep the types and names so upstream compiles as is; the calls succeed but do nothing
// (we do not pretend the priority changed, because it did not). If real priorities are wanted later,
// the right tools are pthread_setschedparam / setpriority -- a performance-tuning concern, not part of
// getting the host to compile.
#pragma once

#include <pthread.h>

typedef enum {
    QOS_CLASS_USER_INTERACTIVE = 0x21,
    QOS_CLASS_USER_INITIATED   = 0x19,
    QOS_CLASS_DEFAULT          = 0x15,
    QOS_CLASS_UTILITY          = 0x11,
    QOS_CLASS_BACKGROUND       = 0x09,
    QOS_CLASS_UNSPECIFIED      = 0x00
} qos_class_t;

static inline int pthread_set_qos_class_self_np(qos_class_t qos_class, int relative_priority) {
    (void)qos_class; (void)relative_priority;
    return 0;
}

static inline int pthread_get_qos_class_np(pthread_t thread, qos_class_t *qos_class, int *relative_priority) {
    (void)thread;
    if (qos_class) *qos_class = QOS_CLASS_UNSPECIFIED;
    if (relative_priority) *relative_priority = 0;
    return 0;
}

static inline int pthread_attr_set_qos_class_np(pthread_attr_t *attr, qos_class_t qos_class, int relative_priority) {
    (void)attr; (void)qos_class; (void)relative_priority;
    return 0;
}

static inline int pthread_attr_get_qos_class_np(pthread_attr_t *attr, qos_class_t *qos_class, int *relative_priority) {
    (void)attr;
    if (qos_class) *qos_class = QOS_CLASS_UNSPECIFIED;
    if (relative_priority) *relative_priority = 0;
    return 0;
}
