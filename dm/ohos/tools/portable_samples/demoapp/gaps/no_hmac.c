// 【反向验证 3/5】CommonCrypto 的 HMAC —— 垫片层只垫了 CommonDigest，**没垫 HMAC**。
// 期望结果：编译探针报 "file not found"（说明"垫了一个头"不等于"垫了整个框架"）。
// [Negative test 3/5] CommonCrypto's HMAC. Only CommonDigest is shimmed. Expected: "file not found" --
// i.e. shimming one header of a framework is not the same as shimming the framework.
#include <CommonCrypto/CommonHMAC.h>
#include <stdint.h>

int demo_gap_hmac(const uint8_t *key, size_t kn, const uint8_t *msg, size_t mn, uint8_t out[32]) {
    CCHmacContext ctx;
    CCHmacInit(&ctx, kCCHmacAlgSHA256, key, kn);
    CCHmacUpdate(&ctx, msg, mn);
    CCHmacFinal(&ctx, out);
    return 0;
}
