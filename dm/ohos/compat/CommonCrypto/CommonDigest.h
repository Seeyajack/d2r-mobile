// 垫片：苹果 <CommonCrypto/CommonDigest.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <CommonCrypto/CommonDigest.h>.
//
// 宿主用它干嘛：算摘要。用的地方不多且都不在热路径上——
//   loader.c      CC_SHA256        —— DM_LOADHASH 诊断（打印模块摘要）
//   diag.c        CC_MD5           —— 诊断
//   hle_tact.c    CC_MD5           —— 手感包/触觉包标识
//   dm_padmode.c  CC_SHA1          —— 取 4 字节短哈希当标识
// 值本身不参与游戏逻辑判断（都是"名称/标识"性质），但仍然必须**算对**，
// 因为对拍（两遍跑结果要逐字节一致）依赖它稳定。
// The host computes digests in a few non-hot paths. The values do not drive game logic, but they must be
// computed correctly because the byte-for-byte comparison runs depend on them being stable.
//
// 实现：本目录 src/dm_compat_hash.c 里是三份标准实现（MD5 / SHA-1 / SHA-256），
// 不依赖 openssl，也不引入第三方库，纯 C、无分配、可重入。
// Implementation: three standard implementations (MD5 / SHA-1 / SHA-256) in src/dm_compat_hash.c --
// no OpenSSL, no third-party library, plain C, allocation-free, reentrant.
//
// 注意：摘要正确性**必须在真机上用标准测试向量验一次**（见 compat/README.md 的"待验"一节），
// 我在本机没有能跑的 C 编译器，不做"应该没问题"的假定。
// Note: digest correctness must be verified once on the device with standard test vectors (see the
// "to be verified" section of compat/README.md); there is no runnable C compiler on this machine, so
// nothing is assumed to be fine.
#pragma once

#include <stdint.h>
#include <stddef.h>

typedef uint32_t CC_LONG;
typedef uint64_t CC_LONG64;

#define CC_MD5_DIGEST_LENGTH     16
#define CC_SHA1_DIGEST_LENGTH    20
#define CC_SHA256_DIGEST_LENGTH  32

typedef struct CC_MD5state_st {
    uint32_t h[4];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} CC_MD5_CTX;

typedef struct CC_SHA1state_st {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} CC_SHA1_CTX;

typedef struct CC_SHA256state_st {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   n;
} CC_SHA256_CTX;

// MD5
unsigned char *CC_MD5(const void *data, CC_LONG len, unsigned char *md);
int CC_MD5_Init(CC_MD5_CTX *c);
int CC_MD5_Update(CC_MD5_CTX *c, const void *data, CC_LONG len);
int CC_MD5_Final(unsigned char *md, CC_MD5_CTX *c);

// SHA-1
unsigned char *CC_SHA1(const void *data, CC_LONG len, unsigned char *md);
int CC_SHA1_Init(CC_SHA1_CTX *c);
int CC_SHA1_Update(CC_SHA1_CTX *c, const void *data, CC_LONG len);
int CC_SHA1_Final(unsigned char *md, CC_SHA1_CTX *c);

// SHA-256
unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md);
int CC_SHA256_Init(CC_SHA256_CTX *c);
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len);
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c);
