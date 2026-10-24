// nv::（NVN 下面的显卡驱动胶水层：建传输内存、连 nvdrv 服务）。NVN 已整块换成宿主空设备，这层无事可做。
// 分配器回调记在宿主全局 g_nv_alloc，图形层以后要按游戏的分配器分内存时从这里取。
// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "../host/hle.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;

#define WHY "NVN 已整块换成宿主空设备，驱动胶水层无事可做"
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "；分配器记入 g_nv_alloc",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "；分配器记入 g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
// np257：**放开 nv::InitializeGraphics，让客体自己的实现跑**。
// 为什么改：上游把它接成空操作，是因为 D2R 的图形全走宿主替换的 NVN，nv:: 这层是"驱动胶水"。
// 但 BotW 的引擎**依赖 nv::InitializeGraphics 把 device 建好并写进全局**——真机证据：
// 开了 DM_NVN_LOG 之后，崩前只有一条 [nvn] 日志，**nvnDeviceInitialize 根本没被调用过**；
// 于是游戏自己的图形设备对象里 +0x1b0 是 NULL，main+0x9fd1d0 在空对象上做虚调用崩掉。
// 放它跑之后，客体会依次调 nvnDeviceBuilder* / nvnDeviceInitialize / nvnWindowBuilder* …，
// 这些都是宿主已实现的。
// ⚠ 若出现"nv:: 期待宿主没提供的东西"，把下面这行恢复回来即可（两个方向都留了注释）。
// HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY, void, h_nv_Init, void *mem, unsigned long size) { (void)mem; (void)size; }
// np257：同理放开（与 InitializeGraphics 成对）
// HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) {}
