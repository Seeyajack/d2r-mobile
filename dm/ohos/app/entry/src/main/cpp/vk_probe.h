// 图形层能力探测：在真机上枚举鸿蒙设备的 Vulkan 能力，回答"这套移植方案能不能做"。
// Graphics capability probe: enumerate the HarmonyOS device's Vulkan capabilities on a real device to
// answer "can this port work at all".
//
// 为什么先做这一步：原实施计划把"Vulkan 能力不足"列为**项目级止损点**——
// 图形层是最大的一块（Metal → Vulkan），如果设备能力覆盖不了需求，整条路线要重新评估。
// 先花几分钟探测，比埋头写几周图形代码再发现做不了，代价小得多。
// Why first: the plan lists "insufficient Vulkan capability" as a project-level stop-loss point. The
// graphics layer is the biggest piece (Metal -> Vulkan); if the device cannot cover the requirements,
// the whole route has to be re-evaluated. A few minutes of probing beats weeks of writing code.
//
// 需求从哪来：dm/gfx/nvn_metal_mapping.zh-CN.md + dm/gfx/README.zh-CN.md（逐条读出来的），
// 不是凭空猜的。每一条需求在下面的 check_* 里都写明了"为什么要它"。
// The requirements come from dm/gfx/nvn_metal_mapping.zh-CN.md and dm/gfx/README.zh-CN.md (read line by
// line), not from guesswork. Each one states why it is needed.
//
// 本探针**不需要窗口**：只做能力枚举，不创建 surface / swapchain，
// 所以可以只按一个按钮就跑，不用先接 XComponent。窗口相关的那部分留到第二阶段。
// This probe needs no window: it only enumerates capabilities and creates no surface or swapchain, so it
// runs from a single button press without wiring up an XComponent first. The window part is phase 2.
#pragma once

#include <stddef.h>

// 跑探测，把可读报告写进 out（含结尾 0）。返回 0 表示跑完了（不代表能力够——够不够看报告文字）。
// Runs the probe and writes a readable report into out (NUL-terminated). 0 means it ran; whether the
// capability is sufficient is a matter of reading the report.
int dm_vk_probe_run(char *out, size_t cap);
