// 摘要：苹果的 CommonCrypto（MD5 / SHA-1 / SHA-256）。
// Digests: Apple's CommonCrypto (MD5 / SHA-1 / SHA-256).
#include "demoapp.h"

#include <CommonCrypto/CommonDigest.h>

void demo_md5(const void *p, size_t n, unsigned char out[16]) {
    CC_MD5(p, (CC_LONG)n, out);
}

void demo_sha1(const void *p, size_t n, unsigned char out[20]) {
    CC_SHA1(p, (CC_LONG)n, out);
}

void demo_sha256(const void *p, size_t n, unsigned char out[32]) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, p, (CC_LONG)n);
    CC_SHA256_Final(out, &c);
}
