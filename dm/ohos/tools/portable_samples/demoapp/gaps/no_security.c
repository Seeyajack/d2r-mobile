// 【反向验证 2/5】Security 框架的安全随机数 —— 垫片层**没垫**。
// 期望结果：编译探针报 "file not found"。
// [Negative test 2/5] The Security framework's secure random, not shimmed.
#include <Security/SecRandom.h>
#include <stdint.h>

int demo_gap_security(void) {
    uint8_t rnd[16];
    return SecRandomCopyBytes(0, sizeof rnd, rnd) == 0;
}
