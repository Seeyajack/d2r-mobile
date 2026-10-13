// 垫片：苹果 <sys/xattr.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <sys/xattr.h>.
//
// 这个垫片的存在理由最"阴险"，值得单独说：
// 鸿蒙（Linux）**也有** <sys/xattr.h>，所以 #include 不会报错——但**函数签名不一样**，
// 编译到调用处才会炸。所以这里不能"发现同名头存在就跳过"，必须自己接管。
// Why this shim is the sneakiest: HarmonyOS (Linux) *also* has <sys/xattr.h>, so the #include succeeds
// -- but the signatures differ, and it only blows up at the call site. So "the header exists, skip it"
// is not an option; we must take it over.
//
//   苹果（宿主用的写法）                                  Linux / 鸿蒙
//   getxattr(p, n, out, sz, position, options)      ←→   getxattr(p, n, out, sz)
//   setxattr(p, n, v, sz, position, options)        ←→   setxattr(p, n, v, sz, flags)
//   removexattr(p, n, options)                      ←→   removexattr(p, n)
//   XATTR_NOFOLLOW                                  ←→   （无；Linux 用 lgetxattr 表示"不跟符号链接"）
//   ENOATTR（属性不存在）                            ←→   ENODATA
//
// 宿主只在一个地方用它（hle_fs.c：在宿主文件上贴一个"客体原名"的属性，用来解决
// APFS 不分大小写 vs Horizon 分大小写的问题）。所以映射很窄，做对这几条就够。
// The host uses it in exactly one place (hle_fs.c tags a host file with the guest's original name, to
// bridge the case-insensitive APFS vs case-sensitive Horizon naming). The mapping surface is tiny.
#pragma once

#include <stddef.h>
#include <sys/types.h>

#define XATTR_NOFOLLOW 0x0001

// 苹果的错误码 ENOATTR 在 Linux 上是 ENODATA（同一个编号 93，名字不同）。
#ifndef ENOATTR
#include <errno.h>
#define ENOATTR ENODATA
#endif

ssize_t getxattr(const char *path, const char *name, void *value, size_t size,
                 uint32_t position, int options);
ssize_t setxattr(const char *path, const char *name, const void *value, size_t size,
                 uint32_t position, int options);
int removexattr(const char *path, const char *name, int options);
