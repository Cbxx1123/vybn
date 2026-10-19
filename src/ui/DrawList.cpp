#include "DrawList.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui {

namespace {

// The area a prim draws, as x0, y0, x1, y1: its rect, grown for a surface as prim.vert grows its
// quad (a raised surface by its shadow, every other surface by the anti-aliased edge's pixel)
void drawnArea(const Prim &prim, float out[4])
{
    float margin = 0.0f;
    if (prim.kind == static_cast<uint32_t>(PrimKind::Surface))
        margin = prim.shape[3] < 0.5f ? prim.shape[1] * 1.4143f + prim.shape[2] + 1.0f : 1.0f;
    out[0] = prim.rect[0] - margin;
    out[1] = prim.rect[1] - margin;
    out[2] = prim.rect[0] + prim.rect[2] + margin;
    out[3] = prim.rect[1] + prim.rect[3] + margin;
}

// Whether two areas (x0, y0, x1, y1) overlap with some area; edges that only touch don't count
bool shares(const float a[4], const float b[4])
{
    return std::min(a[2], b[2]) > std::max(a[0], b[0]) && std::min(a[3], b[3]) > std::max(a[1], b[1]);
}

}

void DrawList::setBounds(const Rect &bounds)
{
    bounds_[0] = bounds.x;
    bounds_[1] = bounds.y;
    bounds_[2] = bounds.x + bounds.w;
    bounds_[3] = bounds.y + bounds.h;
}

bool DrawList::visible(const Prim &prim) const
{
    float area[4];
    drawnArea(prim, area);
    return shares(area, prim.clip) && shares(area, bounds_);
}

void DrawList::add(const Surface &surface)
{
    const Prim prim = surfacePrim(surface, currentClip());
    if (!visible(prim))
        return;
    prims_.push_back(prim);
    ++counts_.surfaces;
}

void DrawList::add(const Text &text)
{
    if (text.text.empty() || !font_)
        return;
    const size_t before = prims_.size();
    glyphPrims(*font_, text, currentClip(), prims_);
    // Keep the glyphs that draw something, in order
    size_t kept = before;
    for (size_t i = before; i < prims_.size(); ++i) {
        if (!visible(prims_[i]))
            continue;
        prims_[kept] = prims_[i];
        ++kept;
    }
    prims_.resize(kept); // shrinking keeps the capacity
    const size_t glyphs = kept - before;
    if (glyphs == 0)
        return;
    ++counts_.texts;
    counts_.glyphs += static_cast<uint32_t>(glyphs);
}

void DrawList::add(const ImageDraw &image)
{
    if (!image.image || image.image->width == 0 || image.image->height == 0)
        return;
    const Prim prim = imagePrim(image, currentClip());
    if (!visible(prim))
        return;
    images_.push_back({static_cast<uint32_t>(prims_.size()), image.image});
    prims_.push_back(prim);
    ++counts_.images;
}

void DrawList::pushClip(const Rect &rect)
{
    Rect clip = rect;
    if (!clipped_.empty()) {
        const Rect &outer = clipped_.back().rect;
        const float x0 = std::max(clip.x, outer.x);
        const float y0 = std::max(clip.y, outer.y);
        const float x1 = std::min(clip.x + clip.w, outer.x + outer.w);
        const float y1 = std::min(clip.y + clip.h, outer.y + outer.h);
        clip = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    Clip entry;
    entry.rect = clip;
    clipBounds(clip, true, entry.bounds);
    clipped_.push_back(entry);
}

void DrawList::popClip()
{
    if (!clipped_.empty())
        clipped_.pop_back();
}

void DrawList::clear()
{
    prims_.clear();
    images_.clear();
    clipped_.clear();
    counts_ = Counts{};
}

namespace {

// One FNV-1a step per 64-bit word of the prim's bytes
uint64_t mixPrim(uint64_t h, const Prim &prim)
{
    static_assert(sizeof(Prim) % 8 == 0, "Prim is read as whole 64-bit words");
    const auto *bytes = reinterpret_cast<const unsigned char *>(&prim);
    for (size_t off = 0; off < sizeof(Prim); off += 8) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + off, sizeof word);
        h = (h ^ word) * 1099511628211ULL;
    }
    return h;
}

}

uint64_t fingerprint(const DrawList &list, uint64_t start)
{
    Fingerprint count(start);
    count.add(static_cast<uint64_t>(list.prims().size()));
    uint64_t h = count.value();
    for (const Prim &prim : list.prims())
        h = mixPrim(h, prim);

    Fingerprint f(h);
    for (const DrawList::ImageRef &ref : list.images()) {
        f.add(ref.prim);
        f.add(ref.image ? ref.image->id : uint64_t{0});
        f.add(ref.image ? ref.image->version : uint64_t{0});
    }
    return f.value();
}

}
