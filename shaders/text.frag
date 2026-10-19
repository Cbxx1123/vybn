#version 450

// Glyph coverage from the font atlas, tinted with the theme's text colour.

layout(set = 0, binding = 0) uniform sampler2D atlas; // single channel: coverage

layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    float coverage = texture(atlas, vUv).r;
    // Colours arrive as sRGB; blending happens in linear space on an sRGB swapchain
    outColor = vec4(pow(vColor.rgb, vec3(2.2)), coverage * vColor.a);
}
