// 文件：苹果的写时复制克隆、原子改名互换、扩展属性（6 参数版）、指令缓存刷。
// Files: Apple's copy-on-write clone, atomic rename swap, extended attributes (the 6-argument form),
// and instruction-cache flushing.
//
// 这几样是"最隐蔽的一类依赖"：`<sys/xattr.h>` 在 Linux 上**也存在**，只是签名不同——
// 不做逐符号普查根本发现不了（见 dm/ohos/compat/README.md）。
// These are the sneakiest kind of dependency: <sys/xattr.h> exists on Linux too, just with different
// signatures -- invisible unless you do a symbol-level census.
#include "demoapp.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/xattr.h>

int demo_clone_tree(const char *src, const char *dst) {
    return clonefile(src, dst, 0);
}

int demo_swap_names(const char *a, const char *b) {
    return renamex_np(a, b, RENAME_SWAP);
}

int demo_tag(const char *path, const char *tag) {
    return setxattr(path, "user.demo.tag", tag, strlen(tag), 0, XATTR_NOFOLLOW);
}

long demo_tag_get(const char *path, char *out, size_t n) {
    return (long)getxattr(path, "user.demo.tag", out, n, 0, XATTR_NOFOLLOW);
}

int demo_untag(const char *path) {
    return (removexattr(path, "user.demo.tag", XATTR_NOFOLLOW) == 0 || errno == ENOATTR) ? 0 : -1;
}

int demo_mtime_sec(const struct stat *st) {
    // 苹果叫 st_mtimespec，Linux 叫 st_mtim——**同物异名**
    return (int)st->st_mtimespec.tv_sec;
}

void demo_flush_code(void *p, size_t n) {
    // 写完代码必须刷指令缓存，否则 CPU 可能执行到旧指令
    sys_icache_invalidate(p, n);
    OSMemoryBarrier();
}
