// 鸿蒙侧的统一日志出口：把上游散落的 LOG()（以及兼容垫片内部的提示）都接到 hilog。
// The unified log sink on HarmonyOS: routes upstream's scattered LOG() calls (and the shim's own notes)
// into hilog.
//
// 为什么不能直接把 fmt 交给 hilog：hilog 的格式化串要求用 %{public}s / %{private}d 这种带可见性
// 标注的写法，而上游的 LOG() 全是普通 printf 风格。所以这里先在本地 vsnprintf 成一行，
// 再整行以 %{public}s 交给 hilog——**上游的格式化串一个字都不用改**。
// Why not hand fmt straight to hilog: hilog wants visibility-annotated specifiers such as %{public}s,
// while upstream's LOG() is all plain printf style. So we vsnprintf locally and pass the finished line
// to hilog as %{public}s -- not one format string upstream has to change.
//
// 怎么接上：构建时定义 DM_LOG_TO_HILOG，`dm/host/dm.h` 就会从这里取 LOG 宏（见该文件的守卫）。
// 不定义时 dm.h 保持原来的 stderr 行为，苹果构建零变化。
// Wiring: define DM_LOG_TO_HILOG at build time and dm/host/dm.h takes the LOG macro from here (see the
// guard in that file). Without it dm.h keeps its original stderr behaviour, so Apple builds are unchanged.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void dm_log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

// 上游 dm.h 直接用它；重复包含时只定义一次。
// Upstream dm.h uses this directly; defined only once even if included repeatedly.
#ifndef DM_LOG_MACRO_DEFINED
#define DM_LOG_MACRO_DEFINED
#define LOG(...) dm_log_printf(__VA_ARGS__)
#endif
