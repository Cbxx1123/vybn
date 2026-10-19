#version 450

// One instance per glyph; six vertices make its quad.

layout(push_constant) uniform Push {
    vec2 screenSize; // framebuffer pixels
} pc;

layout(location = 0) in vec4 inRect;  // x0, y0, x1, y1 in pixels
layout(location = 1) in vec4 inUv;    // u0, v0, u1, v1 in the atlas
layout(location = 2) in vec4 inColor; // sRGB, alpha

layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vColor;

const vec2 corners[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 corner = corners[gl_VertexIndex];
    vec2 pos = mix(inRect.xy, inRect.zw, corner);
    vUv = mix(inUv.xy, inUv.zw, corner);
    vColor = inColor;
    gl_Position = vec4(pos / pc.screenSize * 2.0 - 1.0, 0.0, 1.0);
}
