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

ssize_t dm_compat_getxattr(const char *path, const char *name, void *value, size_t size,
                           uint32_t position, int options);
ssize_t dm_compat_setxattr(const char *path, const char *name, const void *value, size_t size,
                           uint32_t position, int options);
int dm_compat_removexattr(const char *path, const char *name, int options);

// ⚠️ 这里**必须用宏别名，不能直接定义 getxattr 这个名字**。
// 原因是真机上撞出来的：鸿蒙的 libc **也有** getxattr（4 参版）。动态链接时符号按查找顺序抢占，
// libc 排在我们前面 ⇒ 我们的同名定义**永远不会被调到**——哪怕它是强符号、哪怕它就在同一个 .so 里。
// 结果是 hle_fs 一直调到 libc 的 4 参版，错误码也不对（真机上查"属性不存在"返回的是 EOPNOTSUPP
// 而不是 ENOATTR），t_fs_case 的 51 项里 44 项一起挂。
// 用独有名 + 宏别名，调用点在编译期就指向我们，谁也抢不走。
// （strlcpy / arc4random 那几个同名垫片没出事是**运气**：musl 恰好有正确实现、renamex_np 恰好没有。
//  正确姿势是：**凡是 musl 也有、但签名不同的符号，一律独有名 + 宏别名。**）
// ⚠️ The alias macro is mandatory. HarmonyOS's libc *also* exports getxattr (the 4-argument form), and
// dynamic linking resolves by lookup order, with libc ahead of us -- so a same-named definition of ours
// is never called, no matter how strong. Defining distinct names and aliasing via macros binds every
// call site to us at compile time, which nothing can preempt.
#define getxattr      dm_compat_getxattr
#define setxattr      dm_compat_setxattr
#define removexattr   dm_compat_removexattr
