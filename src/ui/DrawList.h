#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include <cstring>
#include <string>
#include <type_traits>

#include "Image.h"
#include "Theme.h"

namespace ui {

// A live image drawn into a rect (pixel for pixel when the rect matches its size)
struct ImageDraw {
    Rect rect;
    const ImageData *image = nullptr; // must outlive the frame's rendering
    float opacity = 1.0f;
};

// Surfaces, text and images collected for one frame, drawn in the order they were added (later on top).
// Consecutive items of one kind with the same clip form a batch, which UIRenderer draws with a
// single call.
struct DrawList {
    enum class Kind { Surface, Text, Image };
    struct Batch {
        Kind kind;
        uint32_t count;
        bool clipped; // false draws to the whole window
        Rect clip;
    };

    std::vector<Surface> surfaces;
    std::vector<Text> texts;
    std::vector<ImageDraw> images;
    std::vector<Batch> batches;

    void add(const Surface &surface)
    {
        surfaces.push_back(surface);
        extend(Kind::Surface);
    }
    void add(const ImageDraw &image)
    {
        if (!image.image || image.image->width == 0 || image.image->height == 0)
            return;
        images.push_back(image);
        extend(Kind::Image);
    }
    void add(Text text)
    {
        if (text.text.empty())
            return;
        texts.push_back(std::move(text));
        extend(Kind::Text);
    }

    // Clips everything added until the matching popClip to `rect`, intersected with any
    // clip already in effect. Shadows are clipped too.
    void pushClip(const Rect &rect)
    {
        Rect clip = rect;
        if (!clips_.empty()) {
            const Rect &outer = clips_.back();
            const float x0 = std::max(clip.x, outer.x);
            const float y0 = std::max(clip.y, outer.y);
            const float x1 = std::min(clip.x + clip.w, outer.x + outer.w);
            const float y1 = std::min(clip.y + clip.h, outer.y + outer.h);
            clip = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
        }
        clips_.push_back(clip);
    }
    void popClip()
    {
        if (!clips_.empty())
            clips_.pop_back();
    }

    void clear()
    {
        surfaces.clear();
        texts.clear();
        images.clear();
        batches.clear();
        clips_.clear();
    }
    bool empty() const { return batches.empty(); }

private:
    void extend(Kind kind)
    {
        const bool clipped = !clips_.empty();
        const Rect clip = clipped ? clips_.back() : Rect{};
        if (!batches.empty()) {
            Batch &last = batches.back();
            const bool sameClip = last.clipped == clipped &&
                                  (!clipped || (last.clip.x == clip.x && last.clip.y == clip.y &&
                                                last.clip.w == clip.w && last.clip.h == clip.h));
            if (last.kind == kind && sameClip) {
                ++last.count;
                return;
            }
        }
        batches.push_back({kind, 1, clipped, clip});
    }

    std::vector<Rect> clips_;
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

// What a frame draws, in order: every batch (its kind, count and clip), then each item of the batch
// with all its fields (a text's characters, an image's id and version). Two frames with the same
// fingerprint draw the same picture, so the second need not be presented. `start` mixes in
// anything else the frame depends on (the clear colour).
inline uint64_t fingerprint(const DrawList &list, uint64_t start = Fingerprint::kOffset)
{
    Fingerprint f(start);
    size_t surfaces = 0, texts = 0, images = 0;
    f.add(static_cast<uint64_t>(list.batches.size()));
    for (const DrawList::Batch &batch : list.batches) {
        f.add(static_cast<int>(batch.kind));
        f.add(batch.count);
        f.add(batch.clipped);
        f.add(batch.clip);
        for (uint32_t i = 0; i < batch.count; ++i) {
            if (batch.kind == DrawList::Kind::Surface && surfaces < list.surfaces.size()) {
                const Surface &s = list.surfaces[surfaces++];
                f.add(s.rect);
                f.add(s.radius);
                f.add(static_cast<int>(s.elevation));
                f.add(s.distance);
                f.add(s.blur);
                f.add(s.base);
                f.add(s.opacity);
                f.add(s.light);
                f.add(s.dark);
                f.add(s.intensity);
                f.add(s.lightDirection);
                f.add(s.curvature);
                f.add(s.rim);
                f.add(s.form);
            } else if (batch.kind == DrawList::Kind::Text && texts < list.texts.size()) {
                const Text &t = list.texts[texts++];
                f.add(t.text);
                f.add(t.baseline);
                f.add(t.size);
                f.add(t.color);
            } else if (batch.kind == DrawList::Kind::Image && images < list.images.size()) {
                const ImageDraw &d = list.images[images++];
                f.add(d.rect);
                f.add(d.opacity);
                // The pixels stay the same until their owner bumps the version
                f.add(d.image ? d.image->id : 0);
                f.add(d.image ? d.image->version : 0);
            }
        }
    }
    return f.value();
}

}
