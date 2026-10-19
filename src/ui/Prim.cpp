#include "Prim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui {

void clipBounds(const Rect &clip, bool clipped, float out[4])
{
    if (!clipped) {
        out[0] = out[1] = -kNoClip;
        out[2] = out[3] = kNoClip;
        return;
    }
    out[0] = std::floor(clip.x);
    out[1] = std::floor(clip.y);
    out[2] = std::ceil(clip.x + clip.w);
    out[3] = std::ceil(clip.y + clip.h);
}

Prim surfacePrim(const Surface &s, const float clip[4])
{
    Prim p{}; // zeroed, pad included
    p.rect[0] = s.rect.x;
    p.rect[1] = s.rect.y;
    p.rect[2] = s.rect.w;
    p.rect[3] = s.rect.h;
    std::memcpy(p.clip, clip, sizeof p.clip);
    p.shape[0] = s.radius;
    p.shape[1] = s.distance;
    p.shape[2] = s.blur;
    p.shape[3] = static_cast<float>(s.elevation) + std::clamp(s.rim, 0.0f, 1.0f) * 0.45f;
    p.base[0] = s.base.r;
    p.base[1] = s.base.g;
    p.base[2] = s.base.b;
    p.base[3] = s.opacity;
    p.light[0] = s.light.r;
    p.light[1] = s.light.g;
    p.light[2] = s.light.b;
    p.light[3] = s.intensity;
    p.dark[0] = s.dark.r;
    p.dark[1] = s.dark.g;
    p.dark[2] = s.dark.b;
    p.dark[3] = s.intensity;
    p.style[0] = s.lightDirection.x;
    p.style[1] = s.lightDirection.y;
    p.style[2] = s.curvature;
    p.style[3] = s.form;
    p.kind = static_cast<uint32_t>(PrimKind::Surface);
    return p;
}

void glyphPrims(const Font &font, const Text &text, const float clip[4], std::vector<Prim> &out)
{
    // Reused between calls; the main thread only
    static std::vector<GlyphQuad> quads;
    quads.clear();
    font.layout(text.text, text.baseline, text.size, quads);
    for (const GlyphQuad &q : quads) {
        Prim p{};
        p.rect[0] = q.x0;
        p.rect[1] = q.y0;
        p.rect[2] = q.x1 - q.x0;
        p.rect[3] = q.y1 - q.y0;
        std::memcpy(p.clip, clip, sizeof p.clip);
        p.shape[0] = q.u0;
        p.shape[1] = q.v0;
        p.shape[2] = q.u1;
        p.shape[3] = q.v1;
        p.base[0] = text.color.r;
        p.base[1] = text.color.g;
        p.base[2] = text.color.b;
        p.base[3] = text.color.a;
        p.kind = static_cast<uint32_t>(PrimKind::Glyph);
        out.push_back(p);
    }
}

Prim imagePrim(const ImageDraw &image, const float clip[4])
{
    Prim p{};
    p.rect[0] = image.rect.x;
    p.rect[1] = image.rect.y;
    p.rect[2] = image.rect.w;
    p.rect[3] = image.rect.h;
    std::memcpy(p.clip, clip, sizeof p.clip);
    p.shape[2] = 1.0f;
    p.shape[3] = 1.0f;
    p.base[0] = 1.0f;
    p.base[1] = 1.0f;
    p.base[2] = 1.0f;
    p.base[3] = image.opacity;
    p.kind = static_cast<uint32_t>(PrimKind::Image);
    return p;
}

}
