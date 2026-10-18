// 鸿蒙侧最小 nn::fs：把客体 romfs 路径映射到宿主目录，只用 POSIX。
// The minimal nn::fs for HarmonyOS: maps guest romfs paths onto a host directory, POSIX only.
//
// 为什么不用上游 dm/host/hle_fs.c：那套是 D2R 专属（APFS 快照式存档事务、rom 覆盖层、
// 注入点、性能计量），依赖 sys/clonefile.h 与 sys/xattr.h（macOS 专有），且存档语义与
// BotW 无关。这里按公开 Horizon ABI 重写最小版：返回码、DirectoryEntry 布局与上游对齐。
// Why not reuse dm/host/hle_fs.c: that one is D2R-specific (APFS snapshot save transactions,
// rom overlays, injection points, profiling) and depends on macOS-only sys/clonefile.h and
// sys/xattr.h. This file is a minimal rewrite against the public Horizon ABI, with the same
// result codes and DirectoryEntry layout.
//
// 数据从哪来：DM_ROMFS 指向宿主目录（默认 /data/local/tmp/romfs）；DM_SAVE 指向存档目录。
// Where the data comes from: DM_ROMFS points at the host directory (default /data/local/tmp/romfs);
// DM_SAVE points at the save directory.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 按导入符号名取真实现；不是本模块管的返回 NULL（调用方落回记录型存根）。
// Returns the real implementation for an imported symbol name, or NULL if not ours.
void *dm_fs_impl(const char *sym_name);

// 日志出口（探针的 klog 接进来，保证与探针日志同一文件）。
void dm_fs_set_log(void (*fn)(const char *, ...));

// 启动自检：打三行日志说明 rom 根能不能读、关键文件在不在。
void dm_fs_selftest(void);

#ifdef __cplusplus
}
#endif
