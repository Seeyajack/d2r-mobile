// dm_nvn_exec_ohos.h —— M4 真执行层：把 CB 记录流翻译成 Vulkan 调用（2026-10-22）
//
// 职责单一：遍历记录流 → 按命令名翻译。当前支持 ClearColor 一族（走已真机验证过的 vk_backend 路径），
// 其余命令先记账不执行（等真机给出真实命令构成再补）。
//
// 判据：dm_nvn_exec_smoketest() 走完 NVN 分发→记录→提交→读回，断言中心像素 = 清屏色。
#pragma once
#include <stdint.h>

int  dm_nvn_exec_ensure(void);        // 懒建 Vulkan 上下文 + 64×64 离屏目标
void dm_nvn_exec_shutdown(void);
int  dm_nvn_exec_submit_cb(void *cb); // 回放一个 CB 的记录流（= 翻译 + 执行）
int  dm_nvn_exec_frame_done(void);    // Present：读回中心像素并与最后清屏色比对
const char *dm_nvn_exec_report(void); // 最近一次判定的人话描述
int  dm_nvn_exec_verdict(void);       // 0 通过 / -1 失败 / -2 未跑
int  dm_nvn_exec_smoketest(void);
void dm_nvn_exec_mark_window(void);   // nvnWindowInitialize 调过 = 需要真上屏     // 合成冒烟（不依赖游戏）
