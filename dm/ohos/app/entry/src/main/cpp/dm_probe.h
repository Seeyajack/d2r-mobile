// 第 1 站：三个真机实验 —— 验证鸿蒙的内存可执行策略与 ELF 段机制。
// 依据：dm/ohos/装载方案-镜像嵌入与定位.md 第四节。
//
//   A  匿名内存能否拿到可执行权限      （预期：失败）
//   B  mmap 自己的 .so 文件能否可执行   （预期：成功 —— 整个方案的基石）
//   C  自定义 ELF 段能否生成 __start_/__stop_ 符号（预期：能）
//
// 结论决定装载方案成立与否，所以这三个实验必须先跑。
#pragma once

#include <stddef.h>

// 依次跑 A/B/C，把可读报告写进 out（含结尾 0）。
// 返回 0 表示三个实验都执行完了（不代表都通过——通过与否看报告文字）。
//
// files_dir：应用的数据目录（ArkTS 侧传 context.filesDir 进来）。
//            实验 B-3 要在里面造个文件试 mmap，用来判断
//            「禁止可执行映射」是针对 bundle 目录还是所有应用文件。
int dm_probe_run(const char *files_dir, char *out, size_t cap);

// np151：线程快照（读 /proc/self/task/*/{stat,wchan,syscall}，只在探针运行期打日志）。
// 由 ArkTS setInterval 每 2 秒调一次——宿主 pthread 永不被调度，只能借 UI 线程进来。
void dm_probe_thread_probe(void);
