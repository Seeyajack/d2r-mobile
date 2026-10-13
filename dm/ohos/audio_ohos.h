// 鸿蒙音频后端（真出声）—— 对应上游 hle_audio.c 里 CoreAudio AudioUnit 的那三处。
// HarmonyOS audio backend (real output) -- the counterpart of the three CoreAudio AudioUnit spots in
// upstream hle_audio.c.
//
// 边界分得很清楚：
//   hle_audio.c 留着：待播 / 已释放队列、按实时消耗的静音接收端、事件通知、全部业务逻辑；
//   本文件只管：建流、启动、停止、释放，以及"该给我多少字节"这件事。
// The boundary is clear: hle_audio.c keeps the queues, the real-time silent sink, the event
// notification and all the business logic; this file owns only open/start/stop/close and how many
// bytes to hand back.
//
// 现状（第一步）：本文件里是**诚实的空实现**——dm_ao_open() 打一句说明后返回 NULL。
//   于是 hle_audio.c 里所有 `if (a->au && ...)` 都自然走静音接收端：
//   鸿蒙侧目前一律静音接收端，**节奏与真机一致，只是没声音**。
// Current state (step 1): honest stubs -- dm_ao_open() logs a note and returns NULL, so every
// `if (a->au && ...)` in hle_audio.c takes the silent-sink path: HarmonyOS currently always uses the
// silent sink, with pacing identical to the real device and no sound.
//
// 为什么返回 NULL 而不是直接 die()：hle_audio.c 对"a->au 为空"本来就有完整分支（走静音接收端），
//   所以 NULL 是最省事也最安全的降级——**不需要改上游任何一处判断**。
// Why NULL instead of die(): hle_audio.c already handles "a->au is NULL" by taking the silent-sink
// branch, so NULL is the least intrusive and safest degradation and needs no upstream change.
//
// 真实现（OHAudio）在第二步填，方案与已核实的接口见 `音频后端方案-从CoreAudio到OHAudio.md`。
// The real OHAudio implementation is step 2; see 音频后端方案-从CoreAudio到OHAudio.md.
#pragma once

// 建流并挂上写数据回调。成功返回流句柄，失败返回 NULL。
// Opens a stream and installs the write-data callback. Returns a handle on success, NULL on failure.
void *dm_ao_open(int channels, int sample_rate);

// 启动 / 停止 / 释放。返回 0 = 成功（与苹果那边"失败返回非 0"的习惯一致）。
// Start / stop / close. Returns 0 on success, matching the Apple-side convention of non-zero failure.
int dm_ao_start(void *handle);
int dm_ao_stop(void *handle);
int dm_ao_close(void *handle);
