// 把上游 dm/host/tests 的单测搬到鸿蒙上跑的框架。
// Framework for running the upstream dm/host/tests unit tests on HarmonyOS.
//
// 为什么值得做：这批单测是**上游作者自己写的、真实的**代码，而且测的是**行为**（不是能不能编译）。
// 它们正好补上"只验证到编译+链接、没验证跑起来"那个空白——垫片的哈希算得对不对、时间对不对、
// 内存读得对不对，都在这批测试的射程之内。
// Why it is worth doing: these tests are real code written by the upstream author and they test
// *behaviour*, not whether things compile.
//
// 为什么是 dlopen 而不是直接链进来：**照上游的结构走**——上游每个单测是一个独立可执行文件。
// 在鸿蒙侧对应"每个单测一个 .so"，脚本按名字 dlopen、取它的 main 来调。
// 试过把 27 个合成一个 .so：会得到 170 个未解析符号（上游在 macOS 靠 -dead_strip 丢掉没用的引用，
// ELF 的 -shared 丢不掉）。详见 dm/ohos/宿主单测搬鸿蒙-方案与现状.md。
// Why dlopen instead of linking them in: follow upstream's structure. Upstream builds one executable per
// test; on HarmonyOS that becomes one .so per test, dlopened by name. Merging all 27 into one .so was
// tried and yields 170 unresolved symbols.
#pragma once

#include <stddef.h>

typedef struct {
    const char *name;   // 显示用（如 t_namegen；同一单测跑多次时带后缀，如 t_i18n[en]）
    const char *lib;    // 不带前缀与后缀的 .so 名（如 dmt_t_namegen），由生成器产出
    const char *arg;    // 传给它的 argv[1]，可为空串
    const char *env;    // "KEY=值;KEY2=值"；值写成 @ 表示"日志文件所在目录"；可为空串
} dm_test_ent;

extern const dm_test_ent dm_host_tests[];
extern const int dm_host_tests_count;

// 顺序跑单测。每跑完一个就把结果**立刻写进 logpath**（并打一份进 hilog），
// 这样即使后面某个单测把进程搞崩了，前面的结果也已经在盘上。
// 报告同时累积进 out（含结尾 0）。返回失败的个数。
//
// filter 决定跑哪些（**调试时很有用**）：
//   "" 或 NULL   —— 续跑：日志里已出结果的跳过
//   "*"          —— 重头跑：先清空日志，全部重跑
//   其它         —— **只跑名字正好等于它的那一个**（忽略日志里的完成标记）
// filter decides which tests run (handy when debugging): empty = resume, "*" = wipe the log and rerun
// everything, anything else = run only the test with that exact name, ignoring the recorded results.
int dm_host_tests_run(const char *logpath, const char *filter, char *out, size_t cap);
