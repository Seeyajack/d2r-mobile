// 由 dm/ohos/tools/mk_romfs_pack.py 产出的归档（romfs 启动子集：System/ 与 Pack/Bootup*），
// 直接嵌进 .so，由 dm_fs_ohos.c 从内存读（鸿蒙沙箱看不到 /data/local/tmp，见该文件顶部注释）。
// 段名 dmromfs 是合法 C 标识符 ⇒ 链接器生成 __start_dmromfs / __stop_dmromfs。
    .section dmromfs, "a", @progbits
    .p2align 4
    .incbin "F:/switch-work/nca_full/romfs_boot.pack"
    .p2align 4
