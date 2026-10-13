// 垫片：苹果 <sys/clonefile.h> 的鸿蒙替身。
// Shim: the HarmonyOS stand-in for Apple's <sys/clonefile.h>.
//
// 宿主用它干嘛（hle_fs.c，只 1 个文件）：存档事务。
//   挂载时：把"已提交目录"整树 clonefile 成工作目录（苹果上 APFS 写时复制，近乎零成本）
//   提交时：把工作目录整树 clonefile 成 .new，再与已提交目录原子互换
//   目的  ：任何时刻被杀，已提交版本要么全是旧版、要么全是新版，不会半截
// Role (hle_fs.c, one file): save transactions -- clone the committed directory tree to a working
// directory at mount; clone the working directory to .new at commit and atomically swap it with the
// committed directory. Killed at any moment, the committed version is all-old or all-new, never half.
//
// 鸿蒙侧：没有写时复制的目录克隆。**功能等价、代价更高**的做法是递归拷贝（见 src/dm_compat_fs.c）。
// 存档目录的量级是几 MB，递归拷贝的代价完全可以接受；语义（克隆后两份互不影响）完全一致。
// HarmonyOS has no copy-on-write directory clone; the functionally equivalent but costlier route is a
// recursive copy (see src/dm_compat_fs.c). Save directories are a few MB, so the cost is acceptable and
// the semantics (the two trees become independent) are identical.
#pragma once

#define CLONE_NOFOLLOW      0x0001
#define CLONE_NOOWNERCOPY   0x0002

int clonefile(const char *src, const char *dst, int flags);
int clonefileat(int src_dirfd, const char *src, int dst_dirfd, const char *dst, int flags);
