// 见 nvn_tex_path.h。纯判断，无状态、无 IO、无平台依赖。
// See nvn_tex_path.h. Pure decision logic: no state, no IO, no platform dependency.
#include "nvn_tex_path.h"

#include "nvn_bc2astc.h"
#include "nvn_bcdec.h"

// BC 一族的"每块字节"：BC1/BC4 是 8，其余（BC2/3/5/6H/7）是 16。
// 这张判断与 nvn_bcdec.c 里那份保持一致——那边按格式号分组，这里按同一组写，改一处要改两处。
static int bc_bpb(uint32_t f) {
    switch (f) {
    case 0x41: case 0x42: case 0x45: case 0x46: case 0x49: case 0x4a: return 8;
    default: return 16;
    }
}

nvn_tex_path nvn_tex_path_choose(uint32_t f, nvn_tex_caps caps) {
    // 不是"能转成 ASTC 的 BC 格式"：要么本来就没压缩，要么是 BC6H 这类 HDR。
    // 这两种都交回调用方的原有路径（未压缩上传），本模块不碰。
    if (!nvn_bc2astc_ok(f)) {
        // 未压缩格式（NVN 自己的 RGBA8/R8/…）本来就走未压缩；BC6H 也走未压缩（半浮点 RGBA16F）。
        uint32_t pf; int bpp;
        return nvn_bc_unpacked(f, &pf, &bpp) ? NVN_TEX_PATH_UNCOMPRESSED : NVN_TEX_PATH_NONE;
    }
    if (caps.bc) return NVN_TEX_PATH_BC_DIRECT;
    if (caps.astc4x4) return NVN_TEX_PATH_ASTC;
    return NVN_TEX_PATH_UNCOMPRESSED;
}

int nvn_tex_path_bpb(uint32_t f, nvn_tex_path path) {
    switch (path) {
    case NVN_TEX_PATH_BC_DIRECT:   return bc_bpb(f);
    case NVN_TEX_PATH_ASTC:        return nvn_bc2astc_bpb(f);   // ASTC 4x4 恒 16
    case NVN_TEX_PATH_UNCOMPRESSED: return 0;                   // 不是块格式
    default:                       return 0;
    }
}

const char *nvn_tex_path_name(nvn_tex_path p) {
    switch (p) {
    case NVN_TEX_PATH_BC_DIRECT:    return "BC 原生直传";
    case NVN_TEX_PATH_ASTC:         return "转码 ASTC 4x4";
    case NVN_TEX_PATH_UNCOMPRESSED: return "解成未压缩";
    case NVN_TEX_PATH_NONE:         return "无路可走";
    default:                        return "未知";
    }
}
