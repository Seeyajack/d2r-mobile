/**
 * libdmprobe —— 第 1 站三个真机实验的 NAPI 接口。
 *
 * 实现：entry/src/main/cpp/dm_probe.c
 * 说明：dm/ohos/装载方案-镜像嵌入与定位.md 第四节
 */

/**
 * 依次跑三个实验，返回可读的报告文本。
 *
 *   C  自定义 ELF 段能否生成 __start_/__stop_ 符号（预期：能）
 *   B  mmap 自己的 .so 文件能否可执行（预期：成功 —— 整个装载方案的基石）
 *      B-3 换应用数据目录的文件再试，分辨禁令范围
 *   A  匿名内存能否拿到可执行权限（预期：失败）
 *
 * @param filesDir 应用数据目录（传 context.filesDir），实验 B-3 要在里面造文件
 */
export const runProbes: (filesDir: string) => string;
