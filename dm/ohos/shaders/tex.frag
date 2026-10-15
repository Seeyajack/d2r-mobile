#version 450

// 最朴素的采样：原样把贴图颜色写出去。
// 采样器在 C 侧配成 NEAREST + CLAMP_TO_EDGE ⇒ 1:1 采样时读到的就是那一个纹素的原始字节，
// 于是"GPU 采到的"可以拿去和"CPU 解出来的"逐像素硬比。
// Plain sampling, no filtering tricks: with NEAREST + CLAMP_TO_EDGE the GPU returns the exact texel, so
// the GPU result can be compared against the CPU decode pixel by pixel.
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uTex;

void main() {
    oColor = texture(uTex, vUV);
}
