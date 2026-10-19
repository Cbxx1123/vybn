#include "DrawList.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace ui {

namespace {

constexpr uint64_t kFnvPrime = 1099511628211ULL;

// Folds one 64-bit word into a running FNV-1a hash
uint64_t foldWord(uint64_t h, uint64_t word)
{
    return (h ^ word) * kFnvPrime;
}

// Folds bytes 64 bits at a time; a last piece shorter than a word is zero-padded to one
uint64_t foldBytes(uint64_t h, const void *data, size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    size_t off = 0;
    for (; off + sizeof(uint64_t) <= size; off += sizeof(uint64_t)) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + off, sizeof word);
        h = foldWord(h, word);
    }
    if (off < size) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + off, size - off);
        h = foldWord(h, word);
    }
    return h;
}

// A prim's bytes (Prim is whole 64-bit words, and zeroed where it has padding)
uint64_t foldPrim(uint64_t h, const Prim &prim)
{
    static_assert(sizeof(Prim) % sizeof(uint64_t) == 0, "Prim is folded as whole 64-bit words");
    return foldBytes(h, &prim, sizeof prim);
}

bool isPlot(const Prim &prim)
{
    return prim.kind == static_cast<uint32_t>(PrimKind::Plot);
}

// The area a prim draws, as x0, y0, x1, y1: its rect, grown for a surface as prim.vert grows its
// quad (a raised surface by its shadow, every other surface by the anti-aliased edge's pixel)
void drawnArea(const Prim &prim, float out[4])
{
    float margin = 0.0f;
    if (prim.kind == static_cast<uint32_t>(PrimKind::Surface))
        margin = prim.shape[3] < 0.5f ? prim.shape[1] * 1.4143f + prim.shape[2] + 1.0f : 1.0f;
    else if (prim.kind == static_cast<uint32_t>(PrimKind::Plot))
        margin = prim.style[0] == 0.0f ? prim.style[1] + 1.0f : 1.0f; // a Line plot's half width
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
    hash_ = foldPrim(hash_, prim);
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
    for (size_t i = before; i < kept; ++i)
        hash_ = foldPrim(hash_, prims_[i]);
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
    hash_ = foldPrim(hash_, prim);
    ++counts_.images;
}

void DrawList::add(const Line &line)
{
    // Its rect is already the area it draws, so it is culled as a glyph is
    const Prim prim = linePrim(line, currentClip());
    if (!visible(prim))
        return;
    prims_.push_back(prim);
    hash_ = foldPrim(hash_, prim);
    ++counts_.lines;
}

void DrawList::addPolyline(const Vec2 *points, size_t count, const Line &style)
{
    for (size_t i = 0; i + 1 < count; ++i) {
        Line segment = style;
        segment.a = points[i];
        segment.b = points[i + 1];
        add(segment);
    }
}

void DrawList::add(const Plot &plot)
{
    if (!plot.values)
        return;
    const bool range = plot.mode == PlotMode::Range;
    if (plot.count < (range ? 1u : 2u))
        return;
    const uint32_t first = static_cast<uint32_t>(plotData_.size());
    const Prim prim = plotPrim(plot, first, currentClip());
    if (!visible(prim))
        return;
    const size_t floats = static_cast<size_t>(plot.count) * (range ? 2 : 1);
    plotData_.insert(plotData_.end(), plot.values, plot.values + floats);
    prims_.push_back(prim);
    // A plot's index is folded relative to its section's values, so the same section hashes the
    // same wherever it lands (painted, or appended at another offset)
    Prim hashed = prim;
    if (!sections_.empty())
        hashed.shape[0] -= static_cast<float>(sections_.back().data);
    hash_ = foldPrim(hash_, hashed);
    hash_ = foldBytes(hash_, plot.values, floats * sizeof(float));
    ++counts_.plots;
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
    plotData_.clear();
    clipped_.clear();
    counts_ = Counts{};
    hash_ = Fingerprint::kOffset;
    sections_.clear();
}

void DrawList::clip(float out[4]) const
{
    std::memcpy(out, currentClip(), sizeof(float) * 4);
}

void DrawList::bounds(float out[4]) const
{
    std::memcpy(out, bounds_, sizeof(float) * 4);
}

void DrawList::beginSection()
{
    sections_.push_back({hash_, prims_.size(), plotData_.size(), images_.size()});
    hash_ = Fingerprint::kOffset;
}

void DrawList::endSection(DrawSection &out)
{
    assert(!sections_.empty() && "endSection without beginSection");
    const OpenSection open = sections_.back();
    sections_.pop_back();

    out.prims.assign(prims_.begin() + static_cast<std::ptrdiff_t>(open.prims), prims_.end());
    out.data.assign(plotData_.begin() + static_cast<std::ptrdiff_t>(open.data), plotData_.end());
    const float dataStart = static_cast<float>(open.data);
    for (Prim &prim : out.prims) {
        if (isPlot(prim))
            prim.shape[0] -= dataStart;
    }
    out.hasImages = images_.size() > open.images;
    out.hash = hash_;
    hash_ = foldWord(open.outerHash, out.hash);
}

void DrawList::appendSection(const DrawSection &section)
{
    assert(!section.hasImages && "a section with images is never appended");
    const float base = static_cast<float>(plotData_.size());
    plotData_.insert(plotData_.end(), section.data.begin(), section.data.end());
    for (Prim prim : section.prims) {
        if (isPlot(prim))
            prim.shape[0] += base;
        prims_.push_back(prim);
        switch (static_cast<PrimKind>(prim.kind)) {
        case PrimKind::Surface: ++counts_.surfaces; break;
        case PrimKind::Glyph: ++counts_.glyphs; break;
        case PrimKind::Line: ++counts_.lines; break;
        case PrimKind::Plot: ++counts_.plots; break;
        case PrimKind::Image: break; // never appended (see DrawSection)
        }
    }
    hash_ = foldWord(hash_, section.hash);
}

uint64_t fingerprint(const DrawList &list, uint64_t start)
{
    Fingerprint f(start);
    f.add(list.hash());
    for (const DrawList::ImageRef &ref : list.images()) {
        f.add(ref.prim);
        f.add(ref.image ? ref.image->id : uint64_t{0});
        f.add(ref.image ? ref.image->version : uint64_t{0});
    }
    return f.value();
}

}
