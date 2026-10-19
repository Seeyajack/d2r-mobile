// 通用 libc 胶水层（L1）：把客体模块**导入的 C 运行库符号**按名字直连宿主实现。
// General libc glue (L1): binds the C-runtime symbols imported by the guest module straight to
// the host implementations, by symbol name.
//
// 为什么必须单独成一层（2026-10-19 真机实证）：
//   旷野之息 main 的 392 个导入里，有一整族是 libc（memcpy/memset/memcmp/str*/math/time）。
//   此前它们全部落到探针的"记录型存根"（只记名、返回 0）⇒ **memcpy 什么都不拷**：
//     任务池记录 memcpy(mgr+0x130, 记录, 0xb8) 之后目标仍是全 0
//     ⇒ 记录 tag=0 ⇒ 建任务分发器返 0 ⇒ RootTask 从未被构造
//     ⇒ RootTask::calc（十阶段启动状态机）从未运行 ⇒ 启动包永不读 ⇒ 内容全空 ⇒ 主循环空转。
//   副证：strcmp 恒返 0 ⇒ 按名字建/查"永远匹配第一个"（假成功）；sin/cos/sqrt 恒 0 ⇒ 几何数据全 0。
// Why a separate layer: a whole family of the guest's 392 imports is libc. They used to fall into
// the probe's recording stub (log the name, return 0) — so memcpy copied nothing, task records came
// out all-zero, RootTask was never constructed, its ten-phase boot machine never ran, and the game
// sat in an empty main loop. Binding these to the host libc is generic (L1): nothing here is
// game-specific.
//
// 与 dm_sdk_ohos.c 的分工：那边管 nn::* 语义（vi/oe/os…），这边只管**纯 C 运行库**。
// Division of labour: dm_sdk_ohos.c owns nn::* semantics; this file owns plain C runtime only.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 按导入符号名取宿主实现；不归本层管返回 NULL（调用方回落记录型存根）。
// Returns the host implementation for an imported symbol name, or NULL if not ours.
void *dm_libc_impl(const char *sym_name);

// 日志出口（探针把 klog 接进来；不接则不打印）。
void dm_libc_set_log(void (*fn)(const char *, ...));

#ifdef __cplusplus
}
#endif
