// 由 dm/ohos/tools/gen_embed_ohos.py 生成，勿手改
// 段名必须是合法 C 标识符：链接器据此生成 __start_<段名> / __stop_<段名>。
// 三块都只要 "a"（可分配）—— 运行时拷进匿名内存再 mprotect 成 R+X。
// ---- 模块 main：TEXT_END 0x1633b30 → 取整 0x1634000，PATCH_OFF 0x2390000，文件 37866096 字节 → 取整 0x2420000 ----
    .section dmmainx0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/main.patched.bin", 0x0, 0x1634000
    .p2align 14
    .section dmmainw, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/main.patched.bin", 0x1634000, 0xd5c000
    .p2align 14
    .section dmmainx1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/main.patched.bin", 0x2390000, 0x8ca70
    .space 0x3590
    .p2align 14
// ---- 模块 sdk：TEXT_END 0x350194 → 取整 0x354000，PATCH_OFF 0x7a0000，文件 8332560 字节 → 取整 0x7f4000 ----
    .section dmsdkx0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/sdk.patched.bin", 0x0, 0x354000
    .p2align 14
    .section dmsdkw, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/sdk.patched.bin", 0x354000, 0x44c000
    .p2align 14
    .section dmsdkx1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/sdk.patched.bin", 0x7a0000, 0x52510
    .space 0x1af0
    .p2align 14
// ---- 模块 rtld：TEXT_END 0x2f3c → 取整 0x4000，PATCH_OFF 0x10000，文件 165168 字节 → 取整 0x2c000 ----
    .section dmrtldx0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/rtld.patched.bin", 0x0, 0x4000
    .p2align 14
    .section dmrtldw, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/rtld.patched.bin", 0x4000, 0xc000
    .p2align 14
    .section dmrtldx1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/rtld.patched.bin", 0x10000, 0x18530
    .space 0x3ad0
    .p2align 14
// ---- 模块 subsdk0：TEXT_END 0xed1ec → 取整 0xf0000，PATCH_OFF 0x200000，文件 2241888 字节 → 取整 0x224000 ----
    .section dmsubsdk0x0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk0.patched.bin", 0x0, 0xf0000
    .p2align 14
    .section dmsubsdk0w, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk0.patched.bin", 0xf0000, 0x110000
    .p2align 14
    .section dmsubsdk0x1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk0.patched.bin", 0x200000, 0x23560
    .space 0xaa0
    .p2align 14
// ---- 模块 subsdk1：TEXT_END 0xffd8 → 取整 0x10000，PATCH_OFF 0x30000，文件 296400 字节 → 取整 0x4c000 ----
    .section dmsubsdk1x0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk1.patched.bin", 0x0, 0x10000
    .p2align 14
    .section dmsubsdk1w, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk1.patched.bin", 0x10000, 0x20000
    .p2align 14
    .section dmsubsdk1x1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk1.patched.bin", 0x30000, 0x185d0
    .space 0x3a30
    .p2align 14
// ---- 模块 subsdk2：TEXT_END 0x2b33c → 取整 0x2c000，PATCH_OFF 0x50000，文件 431744 字节 → 取整 0x6c000 ----
    .section dmsubsdk2x0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk2.patched.bin", 0x0, 0x2c000
    .p2align 14
    .section dmsubsdk2w, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk2.patched.bin", 0x2c000, 0x24000
    .p2align 14
    .section dmsubsdk2x1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk2.patched.bin", 0x50000, 0x19680
    .space 0x2980
    .p2align 14
// ---- 模块 subsdk3：TEXT_END 0x2d9388 → 取整 0x2dc000，PATCH_OFF 0x680000，文件 7239504 字节 → 取整 0x6e8000 ----
    .section dmsubsdk3x0, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk3.patched.bin", 0x0, 0x2dc000
    .p2align 14
    .section dmsubsdk3w, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk3.patched.bin", 0x2dc000, 0x3a4000
    .p2align 14
    .section dmsubsdk3x1, "a", @progbits
    .p2align 14
    .incbin "F:/switch-work/botw/rewritten/subsdk3.patched.bin", 0x680000, 0x67750
    .space 0x8b0
    .p2align 14
