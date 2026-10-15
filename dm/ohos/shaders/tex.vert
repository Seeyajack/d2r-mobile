#version 450

// 全屏三角形：3 个顶点就覆盖整个视口（比"四边形 = 两个三角形"少一截）。
// p 的取值 (0,0) (2,0) (0,2) 同时当 UV 用 —— 视口内的片元 uv 正好落在 [0,1]，
// 且像素中心 (x+0.5)/W 会把 1:1 的贴图采样**精确落在纹素中心**上（判据靠这一点）。
// Full-screen triangle: three vertices cover the whole viewport. p doubles as UV, so a fragment at a
// pixel centre samples exactly at a texel centre -- that is what the pixel-exact check relies on.
layout(location = 0) out vec2 vUV;

void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
