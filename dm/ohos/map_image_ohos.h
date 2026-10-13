// 鸿蒙版 map_image：把 gen_embed_ohos.py 编进 .so 的模块映像放进内存。
//
// 与上游 loader.c 那两套 map_image 的关系：
//   iOS 版   从 Mach-O 段取映像 → vm_allocate + vm_remap（共享已签名页）
//   桌面版   读 <名>.patched.bin → 匿名 mmap + memcpy + mprotect
//   鸿蒙版   从自定义 ELF 段取映像 → 匿名 mmap + memcpy + mprotect
//            ↑ 与桌面版同一思路（真机实验 A 证明匿名 R+X 可用），
//              取映像的方式换成链接器符号（真机实验 C 证明可用）。
//
// 依据：dm/ohos/装载方案-镜像嵌入与定位.md 第二、三节。
//
// 为什么拆成两步：上游桌面版在「拷完」和「封代码区」之间要装键页
// （install_key_page 往映像里写宿主键号，此时内存必须可写），
// 所以这里把「分配+拷贝」和「封权限」分成两个函数，
// 由 loader.c 在中间调 install_key_page —— 顺序与上游完全一致。
#pragma once

#include <stddef.h>
#include "dm.h"

// 第一步：按模块名找到映像段，分配可写匿名内存，把三块按原相对位置拷进去，
// 并填好 m->base / m->size。返回 0 成功，非 0 失败（错误已写 hilog）。
int dm_ohos_map_image_alloc(module_t *m);

// 第二步：把代码区（[0, text_end) 与补丁区）封成只读+可执行。
// 必须在 dm_ohos_map_image_alloc() 与 install_key_page() 之后调。
// 返回 0 成功，非 0 失败。
int dm_ohos_map_image_seal(module_t *m);

// 强制接管表（上游 hle.h 的 HLE_FORCE 宏在鸿蒙侧写进 dmswforce 段）。
// 返回段起始地址并把字节数写进 *nbytes；段不存在时返回 NULL。
const void *dm_ohos_force_table(size_t *nbytes);
