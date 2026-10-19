#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "Font.h"
#include "Image.h"
#include "Prim.h"
#include "Theme.h"

namespace ui {

// What was added between DrawList::beginSection and endSection: its prims, its plot values (the
// prims' first-value indices relative to the section's own data), whether it holds an image, and
// its hash (the running hash of what it added, see DrawList::hash). DrawCache keeps one.
struct DrawSection {
    std::vector<Prim> prims;
    std::vector<float> data;
    bool hasImages = false;
    uint64_t hash = 0;
};

// What one frame draws, as GPU-ready prims in drawing order (later on top, whatever their kind).
// Surfaces become one prim each; text is laid out as it is added, one prim per glyph; images one
// prim each, with the image itself in images(). Each prim carries the clip in effect when it was
// added, so clipping never splits the list (UIRenderer draws it in a few calls).
class DrawList {
public:
    // The font text is laid out with (Context sets its theme's each frame). Text added with no
    // font draws nothing.
    void setFont(const Font *font) { font_ = font; }
    const Font *font() const { return font_; }
    // The window's area (Context sets it each frame). A prim whose drawn area (a raised
    // surface's grown by its shadow, as prim.vert grows it) misses these bounds or its clip is
    // not added: it would draw nothing. Unset, only the clip culls.
    void setBounds(const Rect &bounds);

    void add(const Surface &surface);
    // Laid out now; an empty text adds nothing
    void add(const Text &text);
    // An image with no pixels (null, or zero width or height) adds nothing
    void add(const ImageDraw &image);
    // A line, one prim; a line of zero length is a dot of its width
    void add(const Line &line);
    // A line through `count` points, one Line prim per segment, each with `style`'s width, colour
    // and opacity (style.a and style.b are ignored); fewer than two points add nothing
    void addPolyline(const Vec2 *points, size_t count, const Line &style);
    // A plot, one prim; its values are copied into plotData(). Nothing is added for fewer than 2
    // values (1 for Range) or a null `values`.
    void add(const Plot &plot);
    // Every plot's values in the order added (each Range's as low, high pairs), which the
    // renderer uploads into the shaders' storage buffer
    const std::vector<float> &plotData() const { return plotData_; }

    // Clips everything added until the matching popClip to `rect`, intersected with any clip
    // already in effect. Shadows are clipped too.
    void pushClip(const Rect &rect);
    void popClip();

    // Forgets everything drawn and every clip; keeps the font and the vectors' capacity
    void clear();
    bool empty() const { return prims_.empty(); }

    const std::vector<Prim> &prims() const { return prims_; }
    // Each Image prim's image: the prim's index and the image (which must outlive the frame's
    // rendering), in the order added
    struct ImageRef {
        uint32_t prim;
        const ImageData *image;
    };
    const std::vector<ImageRef> &images() const { return images_; }

    // What was added since the last clear, for FrameStats: surfaces, texts (each add(Text) that
    // drew something), glyphs (their prims) and images
    struct Counts {
        uint32_t surfaces = 0;
        uint32_t texts = 0;
        uint32_t glyphs = 0;
        uint32_t images = 0;
        uint32_t lines = 0; // Line prims added
        uint32_t plots = 0; // Plot prims added
    };
    const Counts &counts() const { return counts_; }

    // The clip prims added now would carry (x0, y0, x1, y1, see clipBounds) and the bounds set
    // by setBounds (the whole plane if unset), for caches' keys
    void clip(float out[4]) const;
    void bounds(float out[4]) const;

    // The running hash of everything added since clear: each prim's bytes and each plot's values
    // are folded in as they are added (64-bit words, FNV-1a style), and a section, painted or
    // appended, as one word, its own hash. So a frame drawn from cached sections hashes the same
    // as one painted afresh with the same content.
    uint64_t hash() const { return hash_; }

    // Sections (they nest): beginSection starts one; endSection copies what was added since into
    // `out` (prims, plot values rebased to the section's start, whether an image was added, and
    // the section's hash) and folds its hash into the enclosing hash as one word. The prims stay
    // in the list. appendSection adds a section's prims and values again (rebasing their plot
    // indices to where its values land), counts them, and folds its hash as one word. Never
    // append a section holding an image (its ImageRefs aren't kept). Unbalanced calls are
    // programming errors.
    void beginSection();
    void endSection(DrawSection &out);
    void appendSection(const DrawSection &section);

private:
    // The clip in effect, as prims carry it (see clipBounds)
    const float *currentClip() const { return clipped_.empty() ? noClip_ : clipped_.back().bounds; }
    // Whether the area a prim draws shares some with its clip and the bounds (edges that only
    // touch don't count); a prim that doesn't is not added
    bool visible(const Prim &prim) const;

    struct Clip {
        Rect rect;       // for intersecting the next one
        float bounds[4]; // rounded out, as prims carry it
    };

    const Font *font_ = nullptr;
    std::vector<Prim> prims_;
    std::vector<ImageRef> images_;
    std::vector<float> plotData_;
    std::vector<Clip> clipped_;
    Counts counts_;
    float noClip_[4] = {-kNoClip, -kNoClip, kNoClip, kNoClip};
    // The window's area as x0, y0, x1, y1 (the whole plane until setBounds)
    float bounds_[4] = {-kNoClip, -kNoClip, kNoClip, kNoClip};
    uint64_t hash_ = 14695981039346656037ULL; // FNV-1a's offset, as Fingerprint::kOffset
    // Open sections: the enclosing hash and where the section's prims, values and images start
    struct OpenSection {
        uint64_t outerHash;
        size_t prims;
        size_t data;
        size_t images;
    };
    std::vector<OpenSection> sections_;
};

// Hashes plain values one field at a time (64-bit FNV-1a), so no padding bytes take part
class Fingerprint {
public:
    static constexpr uint64_t kOffset = 14695981039346656037ULL;

    explicit Fingerprint(uint64_t start = kOffset) : hash_(start) {}

    uint64_t value() const { return hash_; }

    void bytes(const void *data, size_t size)
    {
        const auto *p = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i) {
            hash_ ^= p[i];
            hash_ *= 1099511628211ULL;
        }
    }
    // Integers, enums and booleans
    template <typename T>
    void add(const T &v)
    {
        static_assert(std::is_trivially_copyable<T>::value, "hash plain values only");
        bytes(&v, sizeof v);
    }
    void add(float f)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        add(bits);
    }
    void add(const std::string &s)
    {
        add(static_cast<uint64_t>(s.size()));
        bytes(s.data(), s.size());
    }
    void add(const Vec2 &v)
    {
        add(v.x);
        add(v.y);
    }
    void add(const Rect &r)
    {
        add(r.x);
        add(r.y);
        add(r.w);
        add(r.h);
    }
    void add(const Color &c)
    {
        add(c.r);
        add(c.g);
        add(c.b);
        add(c.a);
    }

private:
    uint64_t hash_;
};

// What a frame draws: the list's running hash (hash(), everything added in order), then each
// image's id and version (its pixels stay the same until its owner bumps the version). Two frames
// with the same fingerprint draw the same picture, so the second need not be presented. `start`
// mixes in anything else the frame depends on (the clear colour).
uint64_t fingerprint(const DrawList &list, uint64_t start = Fingerprint::kOffset);

}
