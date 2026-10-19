#version 450

// Every prim (src/ui/Prim.h), by kind. See prim.vert for the inputs.
// CONTRACT: set 0 binding 0 is the font atlas (sampler2D, single channel coverage), used by glyphs.
// Set 1 binding 0 is the picture an image prim shows (sampler2D, sRGB pixels); UIRenderer binds
// a blank one when no image is drawn.

layout(set = 0, binding = 0) uniform sampler2D atlas; // glyphs: coverage
layout(set = 1, binding = 0) uniform sampler2D image; // images: the picture

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec2 vUv;
layout(location = 2) flat in vec2 vHalfSize;
layout(location = 3) flat in vec4 vShape;
layout(location = 4) flat in vec4 vBase;
layout(location = 5) flat in vec4 vLight;
layout(location = 6) flat in vec4 vDark;
layout(location = 7) flat in vec4 vStyle;
layout(location = 8) flat in uint vKind;

layout(location = 0) out vec4 outColor;

// Signed distance to a rounded rectangle centred on the origin (negative inside)
float roundedRect(vec2 p, vec2 halfSize, float radius) {
    radius = min(radius, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

// Signed distance to a triangle (from Inigo Quilez's 2D distance functions)
float triangle(vec2 p, vec2 p0, vec2 p1, vec2 p2) {
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// Distance to this surface's outline: a rounded rect, a triangle filling the rect, or an X
// corner to corner (with `radius` as half its stroke width)
float shape(vec2 p, vec2 halfSize, float radius) {
    float form = vStyle.w;
    if (form < 0.5)
        return roundedRect(p, halfSize, radius);
    if (form > 2.5 && form < 3.5) {
        // Two strokes along the diagonals of the largest square that fits
        vec2 q = vec2(p.x + p.y, p.x - p.y) * 0.70710678;
        float arm = min(halfSize.x, halfSize.y) * 1.41421356 - radius;
        return min(roundedRect(q, vec2(arm, radius), radius), roundedRect(q, vec2(radius, arm), radius));
    }
    if (form > 4.5) {
        // Pointing up: the right-pointing triangle turned a quarter anticlockwise
        p = vec2(-p.y, p.x);
        halfSize = halfSize.yx;
    } else if (form > 3.5) {
        // Pointing down: the right-pointing triangle with x and y swapped
        p = p.yx;
        halfSize = halfSize.yx;
    } else if (form > 1.5) {
        p.x = -p.x; // pointing left
    }
    vec2 h = halfSize - radius;
    return triangle(p, vec2(-h.x, -h.y), vec2(h.x, 0.0), vec2(-h.x, h.y)) - radius;
}

// Colours arrive as sRGB; blending happens in linear space on an sRGB swapchain
vec3 toLinear(vec3 c) {
    return pow(c, vec3(2.2));
}

// Non-premultiplied "top over bottom"
vec4 over(vec4 top, vec4 bottom) {
    float a = top.a + bottom.a * (1.0 - top.a);
    if (a <= 0.0)
        return vec4(0.0);
    vec3 c = (top.rgb * top.a + bottom.rgb * bottom.a * (1.0 - top.a)) / a;
    return vec4(c, a);
}

void main() {
    float radius = vShape.x;

    // Derivatives of the distance stay outside any branch on the kind. Only surfaces have a
    // distance; the others get zero, and their derivatives are unused.
    float dist = vKind == 0u ? shape(vLocal, vHalfSize, radius) : 0.0;
    vec2 g = vec2(dFdx(dist), dFdy(dist));
    vec2 normal = g / max(length(g), 1e-4); // outward

    // The atlas is sampled here, in uniform control flow, for every kind
    vec4 tex = texture(atlas, vUv);

    if (vKind == 1u) {
        // Glyph: text.frag's body
        float coverage = tex.r;
        // Colours arrive as sRGB; blending happens in linear space on an sRGB swapchain
        outColor = vec4(pow(vBase.rgb, vec3(2.2)), coverage * vBase.a);
        return;
    }

    if (vKind == 2u) {
        // Image: its pixels are sRGB; the capture's alpha byte is unused. textureLod, so the
        // branch needs no derivatives.
        vec3 rgb = textureLod(image, vUv, 0.0).rgb;
        outColor = vec4(pow(rgb, vec3(2.2)), vBase.a);
        return;
    }

    // Surface: ui.frag's body
    vec2 lightDir = vStyle.xy;
    // The dark shadow falls away from the light; sqrt(2) keeps the classic 45-degree look the same length
    vec2 offset = -lightDir * vShape.y * 1.41421356;
    float blur = max(vShape.z, 1.0);
    // The elevation (0 raised, 1 flat, 2 inset) carries the rim's strength in its fraction (0-0.45)
    float elevation = floor(vShape.w + 0.5);
    float rim = clamp((vShape.w - elevation) / 0.45, 0.0, 1.0);

    vec3 base = toLinear(vBase.rgb);
    vec3 light = toLinear(vLight.rgb);
    vec3 dark = toLinear(vDark.rgb);

    // Convex faces brighten toward the light and darken away from it; concave ones the reverse
    float curvature = vStyle.z;
    if (curvature != 0.0) {
        float t = clamp(dot(vLocal / max(vHalfSize, vec2(1.0)), lightDir) * 0.7, -1.0, 1.0) * curvature;
        base = t > 0.0 ? mix(base, light, t) : mix(base, dark, -t);
    }

    float coverage = 1.0 - smoothstep(-0.5, 0.5, dist);

    // Rim: a fine lit edge on the side facing the light and a dark one opposite (raised), or the
    // reverse (inset), for crisp, machined-looking edges.
    if (rim > 0.0 && elevation != 1.0) {
        float edge = 1.0 - smoothstep(0.2, 1.7, -dist);
        float facing = dot(normal, lightDir);
        if (elevation > 1.5)
            facing = -facing;
        vec3 shine = min(light * 1.35 + vec3(0.012), vec3(1.0));
        base = mix(base, shine, edge * rim * max(facing, 0.0) * 0.85);
        base = mix(base, dark, edge * rim * max(-facing, 0.0) * 0.45);
    }

    vec4 color;
    if (elevation < 0.5) {
        // Raised: shadows outside the shape, offset away from (dark) and toward (light) the light source
        float darkAlpha = (1.0 - smoothstep(-blur, blur, shape(vLocal - offset, vHalfSize, radius))) * vDark.a;
        float lightAlpha = (1.0 - smoothstep(-blur, blur, shape(vLocal + offset, vHalfSize, radius))) * vLight.a;
        color = vec4(dark, darkAlpha);
        color = over(vec4(light, lightAlpha), color);
        color = over(vec4(base, coverage), color);
    } else if (elevation < 1.5) {
        // Flat: just the shape
        color = vec4(base, coverage);
    } else {
        // Inset: shadows inside the shape; dark along the edge nearest the light, light along the far edge
        float darkAlpha = smoothstep(-blur, blur, shape(vLocal - offset, vHalfSize, radius)) * vDark.a;
        float lightAlpha = smoothstep(-blur, blur, shape(vLocal + offset, vHalfSize, radius)) * vLight.a;
        vec3 c = mix(base, dark, darkAlpha);
        c = mix(c, light, lightAlpha);
        color = vec4(c, coverage);
    }

    outColor = vec4(color.rgb, color.a * vBase.a);
}
