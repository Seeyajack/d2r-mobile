// nn::vi（显示层）：真机经 vi 服务开显示、建图层、取原生窗口句柄；句柄交给 NVN 的窗口 builder。
// 宿主：返回宿主静态对象；接 Metal 时原生窗口对应前端的 CAMetalLayer（front_metal_layer()）。
// nn::vi (display layer): on a real device, the vi service is used to open the display, create a layer and get the native window handle; the handle is passed to NVN's window builder.
// Host: returns host static objects; when hooking up Metal, the native window corresponds to the front end's CAMetalLayer (front_metal_layer()).
#include <string.h>
#include "../host/hle.h"

static struct { uint32_t magic; } g_display = {0x50534944}, g_layer = {0x5259414c};
static struct { uint32_t magic; void *layer; } g_native = {0x4e49574e, 0};

HLE_FORCE("_ZN2nn2vi10InitializeEv", "显示服务初始化；宿主无 vi 服务", void, h_vi_Init, void) {}
HLE_FORCE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", "开默认显示；宿主给静态对象", Result, h_vi_OpenDisp, void **out) {
    *out = &g_display; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", "建图层；宿主给静态对象", Result, h_vi_CreateLayer, void **out, void *disp) {
    (void)disp; *out = &g_layer; return RES_OK;
}
HLE_FORCE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", "原生窗口句柄，交给 NVN 窗口 builder；宿主给静态对象", Result, h_vi_GetNative, void **out, void *layer) {
    (void)layer; *out = &g_native; return RES_OK;
}

// ── np242：下面两条 BotW 会用到、D2R 不会，原版 hle_vi.c 没有 ──
// 不补的后果（真机实测）：客体自己 subsdk1 里的 vi 实现跑起来，拿着宿主给的"只有 magic 的
// 假 Layer/Display"去调虚函数 ⇒ 空函数指针 ⇒ subsdk1+0xe230 SIGSEGV。
// 所以凡是客体可能调的 vi 入口都要在宿主截住，别让它碰到假对象内部。
HLE_FORCE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",
          "图层缩放模式；宿主不做缩放（真机全屏）", Result, h_vi_SetScaling, void *layer, int mode) {
    (void)layer; (void)mode; return RES_OK;
}
// 显示 vsync 事件：宿主给一个"永不触发"的静态事件（内容清零）。
// ⚠ 只清 0x20 字节 —— nn::os::SystemEventType 在 64 位下是 4×8 字节，宁可少清也不要越界写调用方的栈。
// ⚠ 若游戏是靠这个事件配帧（而不是靠 nvnWindowAcquireTexture 阻塞），它会一直等 ⇒ 表现是"卡住不崩"；
//   真机上若卡在这里，就改成每帧由前端 Set 一次。
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",
          "显示 vsync 事件；宿主给静态事件对象（永不触发）", Result, h_vi_GetVsync, void *ev, void *disp) {
    (void)disp;
    if (ev) memset(ev, 0, 0x20);
    return RES_OK;
}
