// 【反向验证 1/5】苹果的统一日志 os_log —— 垫片层**没垫**。
// 期望结果：编译探针报 "file not found"。
// 这个文件的作用是"反向验证"：如果探针在这里报「通过」，说明探针漏检了，比缺垫片更糟。
// [Negative test 1/5] Apple's unified logging, not shimmed. Expected: "file not found" from the compile
// probe. If the probe reports success here, the probe is broken -- worse than a missing shim.
#include <os/log.h>

int demo_gap_oslog(void) {
    os_log_t l = os_log_create("demo", "gap");
    return l != 0;
}
