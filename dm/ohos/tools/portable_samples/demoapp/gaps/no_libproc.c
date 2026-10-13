// 【反向验证 4/5】libproc 的进程信息 —— 垫片层**没垫**。
// 期望结果：编译探针报 "file not found"。
// 上游 dm/host/diag.c 里有一段注释提到过它（"iOS SDK 没有 libproc"，当时是用别的办法绕的）。
// [Negative test 4/5] libproc process info, not shimmed. Upstream dm/host/diag.c mentions it in a comment
// (the iOS SDK lacks libproc, and it was worked around then).
#include <libproc.h>

int demo_gap_libproc(int pid) {
    struct proc_bsdinfo pi;
    return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &pi, sizeof pi) > 0;
}
