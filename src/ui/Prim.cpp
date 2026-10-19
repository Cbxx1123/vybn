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
    p.base = packColor(s.base.r, s.base.g, s.base.b, s.opacity);
    p.light = packColor(s.light.r, s.light.g, s.light.b, s.intensity);
    p.dark = packColor(s.dark.r, s.dark.g, s.dark.b, s.intensity);
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
        p.base = packColor(text.color.r, text.color.g, text.color.b, text.color.a);
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
    p.base = packColor(1.0f, 1.0f, 1.0f, image.opacity);
    p.kind = static_cast<uint32_t>(PrimKind::Image);
    return p;
}

Prim linePrim(const Line &line, const float clip[4])
{
    Prim p{}; // zeroed, pad included
    const float half = std::max(0.5f, line.width * 0.5f);
    const float grow = half + 1.0f;
    const float x0 = std::min(line.a.x, line.b.x) - grow;
    const float y0 = std::min(line.a.y, line.b.y) - grow;
    const float x1 = std::max(line.a.x, line.b.x) + grow;
    const float y1 = std::max(line.a.y, line.b.y) + grow;
    p.rect[0] = x0;
    p.rect[1] = y0;
    p.rect[2] = x1 - x0;
    p.rect[3] = y1 - y0;
    std::memcpy(p.clip, clip, sizeof p.clip);
    p.shape[0] = line.a.x;
    p.shape[1] = line.a.y;
    p.shape[2] = line.b.x;
    p.shape[3] = line.b.y;
    p.style[0] = half;
    p.base = packColor(line.color.r, line.color.g, line.color.b, line.opacity);
    p.kind = static_cast<uint32_t>(PrimKind::Line);
    return p;
}

}
