// 鸿蒙音频后端（真出声）—— 第一步：空实现。
// HarmonyOS audio backend (real output) -- step 1: stubs.
//
// 这一步的目标只有一个：让上游 hle_audio.c 能编过，且**不改变任何现有行为**。
// 因为 real_audio() 在鸿蒙侧缺省为 0（它判断的是 TARGET_OS_IPHONE，垫片让它等于 0），
// 所以这些函数在缺省配置下**根本不会被调到**；只有用户显式设 DM_AUDIO=1 时才会。
// The only goal of this step: let upstream hle_audio.c compile without changing any existing
// behaviour. Since real_audio() defaults to 0 on HarmonyOS (it tests TARGET_OS_IPHONE, which the shim
// makes 0), these functions are not reached at all in the default configuration -- only when the user
// explicitly sets DM_AUDIO=1.
#include "audio_ohos.h"

#include "dm_log.h"

// 只在"用户显式要求真出声"时才会走到这里；说清楚现状，不要让人以为设了没反应。
// Reached only when the user explicitly asks for real output; be explicit so an enabled switch that
// does nothing is never mistaken for a bug.
static void dm_ao_note_once(void) {
    static int done;
    if (done) return;
    done = 1;
    dm_log_printf("【说明】鸿蒙侧真出声后端尚未实现（第一步）：本次仍走静音接收端——"
                  "节奏与真机一致，但没有声音。方案见 dm/ohos/音频后端方案-从CoreAudio到OHAudio.md");
}

void *dm_ao_open(int channels, int sample_rate) {
    (void)channels; (void)sample_rate;
    dm_ao_note_once();
    return 0;          // 返回 NULL ⇒ hle_audio.c 走它自己的静音接收端分支（那里本就有完整逻辑）
}

int dm_ao_start(void *handle) { (void)handle; return 0; }
int dm_ao_stop(void *handle)  { (void)handle; return 0; }
int dm_ao_close(void *handle) { (void)handle; return 0; }
