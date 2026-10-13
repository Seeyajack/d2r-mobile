// 把上游 dm/host/tests 的单测搬到鸿蒙上跑的框架。
// Framework for running the upstream dm/host/tests unit tests on HarmonyOS.
//
// 为什么值得做：这批单测是**上游作者自己写的、真实的**代码，而且测的是**行为**（不是能不能编译）。
// 它们正好补上"只验证到编译+链接、没验证跑起来"那个空白——垫片的哈希算得对不对、时间对不对、
// 内存读得对不对，都在这批测试的射程之内。
// Why it is worth doing: these tests are real code written by the upstream author and they test
// *behaviour*, not whether things compile. They fill exactly the gap left by "verified to compile and
// link, never verified to run" -- digest correctness, clock behaviour and memory reads are all in range.
//
// 表（哪些单测、各自的入口）由 gen_host_tests.py 从 Makefile 生成，本文件不认识具体哪些单测。
// The roster is generated from the Makefile by gen_host_tests.py; this file knows no specific test.
#pragma once

#include <stddef.h>

typedef struct {
    const char *name;
    int (*fn)(void);      // 各单测的 main 已被改名成这个入口
} dm_test_ent;

extern const dm_test_ent dm_host_tests[];
extern const int dm_host_tests_count;

// 顺序跑所有单测。每跑完一个就把结果**立刻写进 logpath**（并打一份进 hilog），
// 这样即使后面某个单测把进程搞崩了，前面的结果也已经在盘上。
// 报告同时累积进 out（含结尾 0）。返回失败的个数。
// Runs every test in order, writing each result to logpath immediately (and mirroring to hilog), so that
// a later crash cannot lose earlier results. The report also accumulates into out (NUL-terminated).
// Returns the number of failures.
int dm_host_tests_run(const char *logpath, char *out, size_t cap);
