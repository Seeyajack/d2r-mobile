// 垫片：苹果 <mach-o/dyld.h> 的鸿蒙替身（宿主只用到 _NSGetExecutablePath 一个函数）。
// Shim: the HarmonyOS stand-in for Apple's <mach-o/dyld.h>. The host uses exactly one function:
// _NSGetExecutablePath.
//
// 宿主用它干嘛（dm_derived_rt.h）：上游的"壳"包里，从不含游戏内容的安装包中读 dm/derived.tsv。
// 做法是取可执行文件路径 → 砍掉文件名 → 拼上 /dm/derived.tsv。
// Role (dm_derived_rt.h): in the upstream "shell" package, dm/derived.tsv is read from an install
// package that contains no game content. It takes the executable path, drops the file name, and
// appends /dm/derived.tsv.
//
// 鸿蒙侧的现实：**.so 不是一个"可执行文件"，没有对应物**。
// 所以我们不去假装它是，而是**换一个语义明确的落点**：返回 "$DM_ROOT/D2R"。
// 上游拿到后砍掉 "D2R"、拼上 "/dm/derived.tsv"，最终读到 $DM_ROOT/dm/derived.tsv ——
// 也就是"由前端决定资源根目录"，比硬套一个根本不存在的东西诚实。
// DM_ROOT 没设时返回 -1，上游会打印那句"安装包里没有 dm/derived.tsv"的红字并返回 -1，
// 不会崩、也不会读到错的东西。
// Reality on HarmonyOS: a .so is not an "executable file" and has no counterpart. Rather than pretend
// it does, we pick a location with clear semantics: return "$DM_ROOT/D2R", so upstream ends up reading
// $DM_ROOT/dm/derived.tsv -- i.e. the front end decides the resource root. With DM_ROOT unset we return
// -1, upstream prints its red "no dm/derived.tsv in this package" message and returns -1: no crash and
// no wrong data.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 与苹果同名同签名：成功 0；缓冲不够时把需要的大小写进 *bufsize 并返回 -1。
// Same name and signature as Apple: 0 on success; when the buffer is too small it writes the required
// size into *bufsize and returns -1.
static inline int _NSGetExecutablePath(char *buf, uint32_t *bufsize) {
    const char *root = getenv("DM_ROOT");
    if (!root || !*root || !buf || !bufsize) return -1;
    size_t need = strlen(root) + sizeof("/D2R");
    if (*bufsize < need) { *bufsize = (uint32_t)need; return -1; }
    snprintf(buf, *bufsize, "%s/D2R", root);
    return 0;
}
