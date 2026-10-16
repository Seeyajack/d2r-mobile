// 洞内插桩桩（hook stub）：由探针在运行时整段复制进映像的「代码洞」，再把目标函数首指令
// 打 4 字节 b 跳转进来。桩做四件事：保存易失寄存器 → 调宿主记录函数 → 恢复 → 执行原指令 → 回原流程。
// 位置无关：只用 adr / ldr(字面量) / br，整段搬走后语义不变。
// Hook stub: the probe copies this whole blob into the image's code cave at runtime and patches the
// target function's first instruction with a 4-byte "b" into it. The stub saves the volatile
// registers, calls a host logger, restores, executes the original instruction, and jumps back.
// Position independent: only adr / ldr(literal) / br are used, so moving the blob keeps it valid.
#if defined(__OHOS__)
#define CSYM(x) x
#else
#define CSYM(x) _##x
#endif

    .text
    .p2align 3
    .globl CSYM(hook_stub_begin)
CSYM(hook_stub_begin):
    stp x0, x1, [sp, #-0xE0]!
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
    mov x0, sp                      // 宿主入参 = 保存帧指针（宿主从内存读各项，不靠寄存器传值）
    adr x30, 1f                     // 返回点：宿主以 ret 回到这里
    ldr x16, 3f                     // 槽③：宿主记录函数地址（复制时填入）
    br x16
1:
    ldr x9, [sp, #0x90]
    msr nzcv, x9
    ldp x29, x30, [sp, #0xA0]
    ldp x16, x17, [sp, #0x80]
    ldp x14, x15, [sp, #0x70]
    ldp x12, x13, [sp, #0x60]
    ldp x10, x11, [sp, #0x50]
    ldp x8, x9, [sp, #0x40]
    ldp x6, x7, [sp, #0x30]
    ldp x4, x5, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    ldp x0, x1, [sp], #0xE0
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
    stp x0, x1, [sp, #-0xE0]!
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
    mov x0, sp                       // 宿主入参 = 帧指针（宿主读参数、把结果写回 x0）
    adr x30, 1f
    ldr x16, 6f                      // 槽⑥：宿主函数地址
    br x16
1:
    ldr x9, [sp, #0x90]
    msr nzcv, x9
    ldp x29, x30, [sp, #0xA0]
    ldp x16, x17, [sp, #0x80]
    ldp x14, x15, [sp, #0x70]
    ldp x12, x13, [sp, #0x60]
    ldp x10, x11, [sp, #0x50]
    ldp x8, x9, [sp, #0x40]
    ldp x6, x7, [sp, #0x30]
    ldp x4, x5, [sp, #0x20]
    ldp x2, x3, [sp, #0x10]
    add sp, sp, #0xE0                // 不恢复 x0：x0 就是宿主的返回值
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
