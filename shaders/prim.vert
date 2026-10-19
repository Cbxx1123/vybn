#version 450

// Every prim (src/ui/Prim.h): one instance each; six vertices make its quad.
// CONTRACT (the orchestrator's; the shader agent fills in main):
// - push constant: vec2 screenSize, framebuffer pixels
// - per-instance inputs, matching ui::Prim member order (80 bytes):
//     location 0 vec4 inRect    x, y, w, h
//     location 1 vec4 inClip    x0, y0, x1, y1 (whole pixels)
//     location 2 vec4 inShape   Surface: radius, distance, blur, elevation + rim * 0.45; Glyph/Image: u0, v0, u1, v1
//     location 3 vec4 inStyle   Surface: light direction x, y, curvature, form
//     location 4 uvec4 inColors (base, light, dark, kind), each colour packed red in the low byte to
//                               alpha in the high byte (unpackUnorm4x8, sRGB):
//                               base  Surface: colour and opacity; Glyph: colour and alpha; Image: opacity in alpha
//                               light Surface: light shadow rgb, intensity in alpha
//                               dark  Surface: dark shadow rgb, intensity in alpha
//                               kind  0 Surface, 1 Glyph, 2 Image, 3 Line, 4 Plot
// - outputs to prim.frag (the agent may add more, all flat except vLocal and vUv):
//     location 0 vec2 vLocal      Surface: pixel position relative to the rect's centre
//     location 1 vec2 vUv         Glyph/Image: atlas or image coordinates
//     location 2 flat vec2 vHalfSize
//     location 3 flat vec4 vShape
//     location 4 flat vec4 vBase
//     location 5 flat vec4 vLight
//     location 6 flat vec4 vDark
//     location 7 flat vec4 vStyle
//     location 8 flat uint vKind

layout(push_constant) uniform Push {
    vec2 screenSize;
} pc;

layout(location = 0) in vec4 inRect;
layout(location = 1) in vec4 inClip;
layout(location = 2) in vec4 inShape;
layout(location = 3) in vec4 inStyle;
layout(location = 4) in uvec4 inColors;

layout(location = 0) out vec2 vLocal;
layout(location = 1) out vec2 vUv;
layout(location = 2) flat out vec2 vHalfSize;
layout(location = 3) flat out vec4 vShape;
layout(location = 4) flat out vec4 vBase;
layout(location = 5) flat out vec4 vLight;
layout(location = 6) flat out vec4 vDark;
layout(location = 7) flat out vec4 vStyle;
layout(location = 8) flat out uint vKind;

const vec2 corners[6] = vec2[](
    vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
    vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
);

void main() {
    vec2 corner = corners[gl_VertexIndex];
    uint kind = inColors.w;
    vec2 pos;
    if (kind == 0u) {
        // Raised surfaces cast shadows outside their rect, so grow the quad to cover them
        // (the offset is up to distance * sqrt(2) along one axis). Everything else stays inside;
        // 1px covers the anti-aliased edge.
        float margin = inShape.w < 0.5 ? inShape.y * 1.4143 + inShape.z + 1.0 : 1.0;
        pos = inRect.xy - margin + corner * (inRect.zw + 2.0 * margin);
    } else {
        // Glyphs and images: the quad is the rect itself
        pos = mix(inRect.xy, inRect.xy + inRect.zw, corner);
    }

    // Clip: the quad is axis-aligned, so clamping its corners gives its intersection with the
    // clip (or a zero-area quad when they don't meet)
    pos = clamp(pos, inClip.xy, inClip.zw);

    if (kind == 0u) {
        vLocal = pos - (inRect.xy + inRect.zw * 0.5);
        vUv = vec2(0.0);
    } else {
        vLocal = vec2(0.0);
        vUv = mix(inShape.xy, inShape.zw, (pos - inRect.xy) / max(inRect.zw, vec2(1e-6)));
    }

    vHalfSize = inRect.zw * 0.5;
    vShape = inShape;
    vBase = unpackUnorm4x8(inColors.x);
    vLight = unpackUnorm4x8(inColors.y);
    vDark = unpackUnorm4x8(inColors.z);
    vStyle = inStyle;
    vKind = kind;

    gl_Position = vec4(pos / pc.screenSize * 2.0 - 1.0, 0.0, 1.0);
}
