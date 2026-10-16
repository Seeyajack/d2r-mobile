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
    .globl CSYM(hook_stub_end)
CSYM(hook_stub_end):
