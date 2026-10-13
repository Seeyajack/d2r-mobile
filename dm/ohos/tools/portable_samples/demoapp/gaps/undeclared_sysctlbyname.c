// 【反向验证 5/5 —— 这一份最关键】
// 【Negative test 5/5 -- the most important one】
//
// `sysctlbyname` 在苹果的 `<sys/sysctl.h>` 里有声明，而垫片层的 `sys/sysctl.h` **只垫了 `sysctl`**，
// 没声明它。于是会发生一件很坏的事：
//   · **编译探针会报「通过」**——C 只给一个警告（"call to undeclared function"），不报错；
//   · 而编译器会**默认它返回 int**，真正的返回值是 64 位时会被静默截断；
//   · 只有**链接探针**才会把它抓出来（未定义符号 `sysctlbyname`）。
//
// 这正是当初 `clock_gettime_nsec_np` 那个真 bug 的形状（4 个文件在用、不声明也能编过、
// 64 位纳秒被截断）。**所以这一个文件是"为什么必须做链接探针"的活证据。**
//
// In Apple's <sys/sysctl.h> sysctlbyname is declared; the shim's sys/sysctl.h shims only `sysctl` and
// does not declare it. What follows is the bad part: the compile probe reports success (C only warns),
// the compiler assumes it returns int (silently truncating a 64-bit value), and only the link probe
// catches it as an undefined symbol. This is exactly the shape of the real clock_gettime_nsec_np bug --
// which is why this file is living proof that the link probe has to exist.
#include <sys/sysctl.h>
#include <stddef.h>

long long demo_gap_sysctlbyname(void) {
    long long v = 0;
    size_t n = sizeof v;
    if (sysctlbyname("kern.osrevision", &v, &n, NULL, 0)) return -1;
    return v;
}
