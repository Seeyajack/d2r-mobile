// 鸿蒙前端：dm/host/front.h 契约的 HarmonyOS NEXT 实现（与 front_mac.m / front_ios.m 平级）。
// HarmonyOS front end: the HarmonyOS NEXT implementation of the dm/host/front.h contract
// (sibling of front_mac.m / front_ios.m).
//
// 本目录是【设计骨架】：尚未在鸿蒙 NDK 上编译，也未在真机运行。现状与待办见 dm/ohos/README.md。
// This directory is a DESIGN SKELETON: not yet compiled with the HarmonyOS NDK, not yet run on a device.
#pragma once

#include <stdint.h>

// —— 决策 B：不改 front.h ——
// front.h 的 front_metal_layer() 返回 Metal 图层；鸿蒙没有 Metal，本实现让它返回 NULL。
// 图形后端改用下面这个接口拿 OHNativeWindow，由它创建 VkSurfaceKHR。
//
// 生命周期（华为官方文档明确）：OnSurfaceCreated 拿到的 OHNativeWindow 由系统持有一次引用，
// OnSurfaceDestroyed 之后引用减一、可能被释放。所以销毁后本函数必须返回 NULL，
// 图形后端也不得继续持有旧指针。
void *front_ohos_native_window(void);

// ArkTS → 原生 的启动入口（经 NAPI 暴露，名字与 ArkTS 侧封装一致）。
// 做三件事：备好环境（模块目录 / 游戏资源 / 存档 / 临时目录）→ 让宿主装载提前起跑 → 起跑游戏。
// 返回 0 成功，非 0 失败（错误写 hilog）。
//
// 参数都是鸿蒙应用沙箱里的绝对路径：
//   mods  —— 放 *.fixups.tsv / *.gates.tsv 的目录（对应上游 argv[1]；loader.c 的 read_layout /
//            register_gates 总是从这个目录读表）
//   romfs —— 游戏资源目录（对应上游的 DM_ROMFS）
//   save  —— 存档目录（对应 DM_SAVE，上游要求必须给）
//   temp  —— 临时目录（对应 DM_TEMP，可为 NULL）
//
// 注意：模块镜像本身（*.patched.bin）在 iOS 上是「嵌入成签名代码段」由 getsectiondata 取的
// （loader.c 的两套 map_image）；鸿蒙侧要换一种嵌入与定位方式，见 dm/ohos/README.md 的未解决项。
int dm_ohos_boot(const char *mods, const char *romfs, const char *save, const char *temp);

// 当前 XComponent 表面尺寸（像素）。表面没就绪或已销毁时返回 0。
// 触摸回调要用它把触点坐标归一化到 [0,1)。
void front_ohos_surface_size(int *w, int *h);
