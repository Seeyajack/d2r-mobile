// 由 dm/ohos/tools/gen_embed_ohos.py 生成，勿手改
// 段名必须是合法 C 标识符：链接器据此生成 __start_<段名> / __stop_<段名>。
// 三块都只要 "a"（可分配）—— 运行时拷进匿名内存再 mprotect 成 R+X。
// ---- 模块 main：TEXT_END 0x1633b30 → 取整 0x1634000，PATCH_OFF 0x2390000，文件 37784176 字节 → 取整 0x240c000 ----
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
    .incbin "F:/switch-work/botw/rewritten/main.patched.bin", 0x2390000, 0x78a70
    .space 0x3590
    .p2align 14
