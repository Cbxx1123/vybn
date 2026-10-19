#version 450

// Every prim (src/ui/Prim.h), by kind. See prim.vert for the inputs.
// CONTRACT: set 0 binding 0 is the font atlas (sampler2D, single channel coverage), used by glyphs.
// Set 0 binding 1 is the plot data (a readonly float buffer, the values of every plot in the frame, in order);
// a plot (kind 4) reads its values from vShape.x (first) for vShape.y (count; pairs for Range).
// Set 1 binding 0 is the picture an image prim shows (sampler2D, sRGB pixels); UIRenderer binds
// a blank one when no image is drawn.
// Lines (kind 3) use no texture: vLocal is the absolute pixel position, vShape.xy and vShape.zw are
// the segment's ends, vStyle.x is the half width, and coverage is the capsule's distance (as surfaces' edge).
// Plots (kind 4) use no texture either: vLocal is relative to the plot's top-left, vHalfSize is its full
// size, vShape is first, count, low, high, and vStyle is mode (0 Line, 1 Fill, 2 Range), half width (Line)
// or barFraction (Range), and baseline (Fill). Values map to y as rect.y + h * (high - v) / (high - low).

layout(set = 0, binding = 0) uniform sampler2D atlas; // glyphs: coverage
layout(set = 0, binding = 1) readonly buffer PlotData { float values[]; } plotData; // plots: their values
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

// The y of value v in a plot h pixels high (low at the bottom, high at the top); a zero range counts as 1
float valueY(float v, float h) {
    float range = vShape.w - vShape.z;
    if (abs(range) < 1e-9)
        range = 1.0;
    return h * (vShape.w - v) / range;
}

// The y of the plot's value k (clamped to its own values) in a plot h pixels high
float plotY(uint first, uint count, float k, float h) {
    float i = clamp(k, 0.0, float(count - 1u));
    return valueY(plotData.values[first + uint(i)], h);
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

    if (vKind == 3u) {
        // Line: the capsule around the segment from vShape.xy to vShape.zw, half-width vStyle.x
        vec2 a = vShape.xy;
        vec2 ba = vShape.zw - a;
        vec2 pa = vLocal - a;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        float d = length(pa - ba * h) - vStyle.x;
        float coverage = 1.0 - smoothstep(-0.5, 0.5, d);
        outColor = vec4(pow(vBase.rgb, vec3(2.2)), coverage * vBase.a);
        return;
    }

    if (vKind == 4u) {
        // Plot: vLocal from the plot's top-left, vHalfSize its full size. Its values are
        // first..first+count (pairs for Range), each read clamped to them.
        vec2 p = vLocal;
        vec2 size = max(vHalfSize, vec2(1.0));
        uint first = uint(vShape.x);
        uint count = uint(vShape.y);
        float mode = floor(vStyle.x + 0.5);
        float coverage = 0.0;

        if (count == 0u) {
            outColor = vec4(0.0);
            return;
        }

        if (mode > 1.5) {
            // Range: count columns, each a bar from its low to its high, centred in its column
            float colW = size.x / float(count);
            float c = clamp(floor(p.x / colW), 0.0, float(count - 1u));
            uint pair = min(uint(c), count - 1u);
            float lowV = plotData.values[first + 2u * pair];
            float highV = plotData.values[first + 2u * pair + 1u];
            float yA = valueY(highV, size.y);
            float yB = valueY(lowV, size.y);
            float y0 = min(yA, yB);
            float y1 = max(yA, yB);
            if (y1 - y0 < 1.0) {
                // At least 1 px tall, grown about its middle
                float mid = 0.5 * (y0 + y1);
                y0 = mid - 0.5;
                y1 = mid + 0.5;
            }
            float w = max(colW * vStyle.y, 1.0);
            float cx = (c + 0.5) * colW;
            float x0 = cx - 0.5 * w;
            float x1 = cx + 0.5 * w;
            coverage = clamp(min(p.x - x0, x1 - p.x) + 0.5, 0.0, 1.0)
                     * clamp(min(p.y - y0, y1 - p.y) + 0.5, 0.0, 1.0);
        } else if (count >= 2u) {
            float steps = float(count - 1u);
            float spacing = size.x / steps; // x of value k is k * spacing

            if (mode < 0.5) {
                // Line: the distance from the pixel to the curve, a bar where values share a pixel
                float dd;
                if (steps / size.x > 1.0) {
                    // More than one value per pixel: the bar between the lowest and highest values whose
                    // x falls in this pixel's column, sampled at most 64 of them, plus the two neighbours
                    float kLo = max(ceil((p.x - 0.5) / spacing), 0.0);
                    float kHi = min(floor((p.x + 0.5) / spacing), steps);
                    float n = max(kHi - kLo + 1.0, 0.0);
                    float stride = max(ceil(n / 64.0), 1.0);
                    float minY = 1e9;
                    float maxY = -1e9;
                    for (int j = 0; j < 64; j++) {
                        float k = kLo + float(j) * stride;
                        if (k > kHi)
                            break;
                        float y = plotY(first, count, k, size.y);
                        minY = min(minY, y);
                        maxY = max(maxY, y);
                    }
                    float yl = plotY(first, count, kLo - 1.0, size.y);
                    float yr = plotY(first, count, kHi + 1.0, size.y);
                    minY = min(min(minY, yl), yr);
                    maxY = max(max(maxY, yl), yr);
                    dd = max(max(minY - p.y, p.y - maxY), 0.0);
                } else {
                    // The nearest of the segments around this pixel (segment s runs from value s to s + 1)
                    float kc = floor(p.x / spacing);
                    dd = 1e9;
                    for (int d = -1; d <= 1; d++) {
                        float s = clamp(kc + float(d), 0.0, steps - 1.0);
                        vec2 a = vec2(s * spacing, plotY(first, count, s, size.y));
                        vec2 b = vec2((s + 1.0) * spacing, plotY(first, count, s + 1.0, size.y));
                        vec2 ba = b - a;
                        vec2 pa = p - a;
                        float t = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
                        dd = min(dd, length(pa - ba * t));
                    }
                }
                coverage = 1.0 - smoothstep(-0.5, 0.5, dd - vStyle.y);
            } else {
                // Fill: the area between the curve and the baseline, its edges anti-aliased
                if (p.x >= 0.0 && p.x <= size.x) {
                    float fx = clamp(p.x / spacing, 0.0, steps);
                    float k0 = floor(fx);
                    float curveY = mix(plotY(first, count, k0, size.y), plotY(first, count, k0 + 1.0, size.y), fx - k0);
                    float baseY = valueY(vStyle.z, size.y);
                    float top = min(curveY, baseY);
                    float bottom = max(curveY, baseY);
                    coverage = clamp(min(p.y - top, bottom - p.y) + 0.5, 0.0, 1.0);
                }
            }
        }

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
