// 洞内插桩桩（hook stub）：由探针在运行时整段复制进映像的「代码洞」，再把目标函数首指令
// 打 4 字节 b 跳转进来。桩做四件事：保存易失寄存器 → 调宿主记录函数 → 恢复 → 执行原指令 → 回原流程。
// 位置无关：只用 adr / ldr(字面量) / br，整段搬走后语义不变。
// Hook stub: the probe copies this whole blob into the image's code cave at runtime and patches the
// target function's first instruction with a 4-byte "b" into it. The stub saves the volatile
// registers, calls a host logger, restores, executes the original instruction, and jumps back.
// Position independent: only adr / ldr(literal) / br are used, so moving the blob keeps it valid.
//
// ⚠ 为什么连 **v0–v31（32 个向量寄存器，512 字节）** 都要存：
//   宿主是 C 函数，按 AAPCS 它可以随意用 v0–v7、v16–v31，以及 v8–v15 的高 64 位。
//   客体（游戏代码）大量用 NEON（真机崩点 0x9ef9a8 就是一条 `fmul v0.4s`，浮点不碰内存，
//   却缺页 ⇒ 向量状态被毁）。不存向量寄存器，等于每次插桩都在偷偷改游戏的算术状态。
//   代价：桩变长（步长随之变大），代码洞从 ~455 格降到 ~120 格 —— 够用。
#if defined(__OHOS__)
#define CSYM(x) x
#else
#define CSYM(x) _##x
#endif

#define FRAME_SIZE 0x2E0          // x0–x17 + nzcv + x29/x30 + 站点 + 512 字节向量
#define VEC_OFF    0xC0           // 向量寄存器的起始偏移（32×16 = 0x200，到 0x2C0）

    .text
    .p2align 3
    .globl CSYM(hook_stub_begin)
CSYM(hook_stub_begin):
    sub sp, sp, #FRAME_SIZE          // 开栈：x 寄存器的 stp 立即数上限 ±504，装不下 0x2E0，用 sub
    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    mrs x9, nzcv
    str x9, [sp, #0x90]
    stp x29, x30, [sp, #0xA0]
    ldr x9, 4f                      // 槽④：本站点地址（安装器填入，供日志识别）
    str x9, [sp, #0xB0]
    stp q0, q1, [sp, #VEC_OFF + 0x000]     // ★ 向量寄存器 32 个（512 字节）
    stp q2, q3, [sp, #VEC_OFF + 0x020]
    stp q4, q5, [sp, #VEC_OFF + 0x040]
    stp q6, q7, [sp, #VEC_OFF + 0x060]
    stp q8, q9, [sp, #VEC_OFF + 0x080]
    stp q10, q11, [sp, #VEC_OFF + 0x0A0]
    stp q12, q13, [sp, #VEC_OFF + 0x0C0]
    stp q14, q15, [sp, #VEC_OFF + 0x0E0]
    stp q16, q17, [sp, #VEC_OFF + 0x100]
    stp q18, q19, [sp, #VEC_OFF + 0x120]
    stp q20, q21, [sp, #VEC_OFF + 0x140]
    stp q22, q23, [sp, #VEC_OFF + 0x160]
    stp q24, q25, [sp, #VEC_OFF + 0x180]
    stp q26, q27, [sp, #VEC_OFF + 0x1A0]
    stp q28, q29, [sp, #VEC_OFF + 0x1C0]
    stp q30, q31, [sp, #VEC_OFF + 0x1E0]
    mov x0, sp                      // 宿主入参 = 保存帧指针（宿主从内存读各项，不靠寄存器传值）
    adr x30, 1f                     // 返回点：宿主以 ret 回到这里
    ldr x16, 3f                     // 槽③：宿主记录函数地址（复制时填入）
    br x16
1:
    ldr x9, [sp, #0x90]
    msr nzcv, x9
    ldp q30, q31, [sp, #VEC_OFF + 0x1E0]
    ldp q28, q29, [sp, #VEC_OFF + 0x1C0]
    ldp q26, q27, [sp, #VEC_OFF + 0x1A0]
    ldp q24, q25, [sp, #VEC_OFF + 0x180]
    ldp q22, q23, [sp, #VEC_OFF + 0x160]
    ldp q20, q21, [sp, #VEC_OFF + 0x140]
    ldp q18, q19, [sp, #VEC_OFF + 0x120]
    ldp q16, q17, [sp, #VEC_OFF + 0x100]
    ldp q14, q15, [sp, #VEC_OFF + 0x0E0]
    ldp q12, q13, [sp, #VEC_OFF + 0x0C0]
    ldp q10, q11, [sp, #VEC_OFF + 0x0A0]
    ldp q8, q9, [sp, #VEC_OFF + 0x080]
    ldp q6, q7, [sp, #VEC_OFF + 0x060]
    ldp q4, q5, [sp, #VEC_OFF + 0x040]
    ldp q2, q3, [sp, #VEC_OFF + 0x020]
    ldp q0, q1, [sp, #VEC_OFF + 0x000]
    ldp x29, x30, [sp, #0xA0]
    ldp x16, x17, [sp, #0x80]
    ldp x14, x15, [sp, #0x70]
    ldp x12, x13, [sp, #0x60]
    ldp x10, x11, [sp, #0x50]
    ldp x8, x9, [sp, #0x40]
    ldp x6, x7, [sp, #0x30]
    ldp x4, x5, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    ldp x0, x1, [sp, #0x00]
    add sp, sp, #FRAME_SIZE          // 收栈
    .p2align 3
    .globl CSYM(hook_slot_orig)
CSYM(hook_slot_orig):
2:  nop                             // 槽①：原指令（复制时按站点填入）
    .globl CSYM(hook_slot_back)
CSYM(hook_slot_back):
    b 2b                            // 槽②：回原流程（复制时改编码）
    .p2align 3
    .globl CSYM(hook_slot_host)
CSYM(hook_slot_host):
3:  .quad 0                         // 槽③：宿主函数地址（复制时填入）
    .globl CSYM(hook_slot_site)
CSYM(hook_slot_site):
4:  .quad 0                         // 槽④：站点地址（安装器填入）
    .globl CSYM(hook_stub_end)
CSYM(hook_stub_end):

// ===== 替换式桩：宿主**替函数干活**（返回值经 x0 带回），桩**直接返回调用者**，原实现一条都不执行 =====
// 用途：分配器这类"必须真给内存"的函数——绕开"注入被改写/路径分支"等不确定性。
// ⚠ 这里必须是 ret（回 x30 = 调用点的下一条），**不能**跳"站点+4"——被替换的是**函数入口**，
//   站点+4 就是原实现的第二条指令，跳过去等于"宿主跑完、原实现接着跑"（双执行）：
//   首条 stp x20,x19,[sp,#-0x20]! 被跳过 ⇒ 栈帧少 0x20、mov w19,w2 写坏 x19，
//   最后返回的 x0 是原实现的结果，宿主的指针被丢掉。2026-10-16 真机实证（x0/x23 双 0）。
    .p2align 3
    .globl CSYM(hook_rep_begin)
CSYM(hook_rep_begin):
    sub sp, sp, #FRAME_SIZE          // 同记录式桩：用 sub 开栈
    stp x0, x1, [sp, #0x00]
    stp x2, x3, [sp, #0x10]
    stp x4, x5, [sp, #0x20]
    stp x6, x7, [sp, #0x30]
    stp x8, x9, [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x29, x30, [sp, #0xA0]
    mrs x9, nzcv
    str x9, [sp, #0x90]
    ldr x9, 5f                       // 槽⑤：站点地址（日志识别用）
    str x9, [sp, #0xB0]
    stp q0, q1, [sp, #VEC_OFF + 0x000]     // ★ 同记录式桩：向量寄存器也要存
    stp q2, q3, [sp, #VEC_OFF + 0x020]
    stp q4, q5, [sp, #VEC_OFF + 0x040]
    stp q6, q7, [sp, #VEC_OFF + 0x060]
    stp q8, q9, [sp, #VEC_OFF + 0x080]
    stp q10, q11, [sp, #VEC_OFF + 0x0A0]
    stp q12, q13, [sp, #VEC_OFF + 0x0C0]
    stp q14, q15, [sp, #VEC_OFF + 0x0E0]
    stp q16, q17, [sp, #VEC_OFF + 0x100]
    stp q18, q19, [sp, #VEC_OFF + 0x120]
    stp q20, q21, [sp, #VEC_OFF + 0x140]
    stp q22, q23, [sp, #VEC_OFF + 0x160]
    stp q24, q25, [sp, #VEC_OFF + 0x180]
    stp q26, q27, [sp, #VEC_OFF + 0x1A0]
    stp q28, q29, [sp, #VEC_OFF + 0x1C0]
    stp q30, q31, [sp, #VEC_OFF + 0x1E0]
    mov x0, sp                       // 宿主入参 = 帧指针（宿主读参数、把结果写回 x0）
    adr x30, 1f
    ldr x16, 6f                      // 槽⑥：宿主函数地址
    br x16
1:
    ldr x9, [sp, #0x90]
    msr nzcv, x9
    ldp q30, q31, [sp, #VEC_OFF + 0x1E0]
    ldp q28, q29, [sp, #VEC_OFF + 0x1C0]
    ldp q26, q27, [sp, #VEC_OFF + 0x1A0]
    ldp q24, q25, [sp, #VEC_OFF + 0x180]
    ldp q22, q23, [sp, #VEC_OFF + 0x160]
    ldp q20, q21, [sp, #VEC_OFF + 0x140]
    ldp q18, q19, [sp, #VEC_OFF + 0x120]
    ldp q16, q17, [sp, #VEC_OFF + 0x100]
    ldp q14, q15, [sp, #VEC_OFF + 0x0E0]
    ldp q12, q13, [sp, #VEC_OFF + 0x0C0]
    ldp q10, q11, [sp, #VEC_OFF + 0x0A0]
    ldp q8, q9, [sp, #VEC_OFF + 0x080]
    ldp q6, q7, [sp, #VEC_OFF + 0x060]
    ldp q4, q5, [sp, #VEC_OFF + 0x040]
    ldp q2, q3, [sp, #VEC_OFF + 0x020]
    ldp q0, q1, [sp, #VEC_OFF + 0x000]
    ldp x29, x30, [sp, #0xA0]
    ldp x16, x17, [sp, #0x80]
    ldp x14, x15, [sp, #0x70]
    ldp x12, x13, [sp, #0x60]
    ldp x10, x11, [sp, #0x50]
    ldp x8, x9, [sp, #0x40]
    ldp x6, x7, [sp, #0x30]
    ldp x4, x5, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    add sp, sp, #FRAME_SIZE           // 不恢复 x0：x0 就是宿主的返回值
    .globl CSYM(hook_rep_back)
CSYM(hook_rep_back):
    ret                              // 回调用者（x30 = 调用点下一条）；宿主已把结果放进 x0
    .p2align 3
    .globl CSYM(hook_rep_host)
CSYM(hook_rep_host):
6:  .quad 0                          // 槽⑥：宿主函数地址
    .globl CSYM(hook_rep_site)
CSYM(hook_rep_site):
5:  .quad 0                          // 槽⑤：站点地址
    .globl CSYM(hook_rep_end)
CSYM(hook_rep_end):
