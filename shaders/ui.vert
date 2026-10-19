#version 450

// One instance per neumorphic surface; six vertices make its quad.

layout(push_constant) uniform Push {
    vec2 screenSize; // framebuffer pixels
} pc;

layout(location = 0) in vec4 inRect;   // x, y, width, height in pixels
layout(location = 1) in vec4 inShape;  // corner radius, shadow distance, blur, elevation (0 raised, 1 flat, 2 inset)
layout(location = 2) in vec4 inBase;   // surface colour (sRGB), opacity
layout(location = 3) in vec4 inLight;  // light shadow colour (sRGB), intensity
layout(location = 4) in vec4 inDark;   // dark shadow colour (sRGB), intensity
layout(location = 5) in vec4 inStyle;  // light direction (unit, y down), curvature, form (0 rect, 1 triangle right, 2 triangle left, 3 cross, 4 triangle down, 5 triangle up)

layout(location = 0) out vec2 vLocal;       // pixel position relative to the rect centre
layout(location = 1) flat out vec2 vHalfSize;
layout(location = 2) flat out vec4 vShape;
layout(location = 3) flat out vec4 vBase;
layout(location = 4) flat out vec4 vLight;
layout(location = 5) flat out vec4 vDark;
layout(location = 6) flat out vec4 vStyle;

const vec2 corners[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    // Raised surfaces cast shadows outside their rect, so grow the quad to cover them
    // (the offset is up to distance * sqrt(2) along one axis). Everything else stays inside;
    // 1px covers the anti-aliased edge.
    float margin = inShape.w < 0.5 ? inShape.y * 1.4143 + inShape.z + 1.0 : 1.0;

    vec2 pos = inRect.xy - margin + corners[gl_VertexIndex] * (inRect.zw + 2.0 * margin);
    vLocal = pos - (inRect.xy + inRect.zw * 0.5);
    vHalfSize = inRect.zw * 0.5;
    vShape = inShape;
    vBase = inBase;
    vLight = inLight;
    vDark = inDark;
    vStyle = inStyle;

    gl_Position = vec4(pos / pc.screenSize * 2.0 - 1.0, 0.0, 1.0);
}
