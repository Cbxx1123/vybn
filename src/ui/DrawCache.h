#pragma once

#include <cstdint>
#include <utility>

#include "DrawList.h"
#include "Theme.h"

namespace ui {

// Keeps what a component drew while what it depends on stays the same: a repaint boundary, as
// in Flutter or Chromium's paint chunks. A heavy view holds one as a member and draws its slow,
// steady part through it, then what changes every frame (a playhead) on top.
//
//     cache_.draw(list, theme, rect_, key, [&] { drawContent(list, theme); });
//
// `key` must cover everything the paint function reads (the store's revisions, scroll, zoom,
// hover...). The cache adds the rest itself: `rect`, the list's clip and bounds, the theme's
// fingerprint and the font. While they all match last time's, the section painted then is
// appended again (DrawList::appendSection) instead of painting. `reuse` false always paints
// (while recording, while something glides). A section holding an image is never reused.
//
// With VIBIN_VERIFY_CACHE=1 every draw paints; when the key matched, what was painted is
// compared with the cached section, and the first time they differ the cache prints
// "DrawCache: <name> drew something else under the same key" to stderr. Main thread only.
class DrawCache {
public:
    explicit DrawCache(const char *name) : name_(name) {}

    template <typename Paint>
    void draw(DrawList &list, const Theme &theme, const Rect &rect, uint64_t key, Paint &&paint,
              bool reuse = true) const
    {
        const uint64_t full = fullKey(list, theme, rect, key);
        const bool match = valid_ && reuse && full == key_ && !section_.hasImages;
        if (match && !verifying()) {
            list.appendSection(section_);
            counted(true, section_.prims.size());
            return;
        }
        list.beginSection();
        std::forward<Paint>(paint)();
        if (match) {
            DrawSection fresh;
            list.endSection(fresh);
            check(fresh);
            section_ = std::move(fresh);
            counted(true, section_.prims.size());
            return;
        }
        list.endSection(section_);
        key_ = full;
        valid_ = true;
        counted(false, section_.prims.size());
    }

    // Paints next time whatever the key
    void invalidate() const { valid_ = false; }

    // This cache's draws that appended the section again, and that painted
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }

    // Every cache's hits, misses and prims appended from a cache since the last call, and
    // zeroed (Context takes them into FrameStats each frame)
    struct Totals {
        uint32_t hits = 0;
        uint32_t misses = 0;
        uint32_t cachedPrims = 0;
    };
    static Totals takeTotals();

    // Whether VIBIN_VERIFY_CACHE is set (to anything but 0); read once
    static bool verifying();

private:
    // `key` mixed with the rect, the list's clip and bounds, the theme's fingerprint and the font
    static uint64_t fullKey(const DrawList &list, const Theme &theme, const Rect &rect, uint64_t key);
    // Verifying: compares a fresh paint with the cached section, warning once if they differ
    void check(const DrawSection &fresh) const;
    void counted(bool hit, size_t prims) const;

    const char *name_;
    mutable DrawSection section_;
    mutable uint64_t key_ = 0;
    mutable bool valid_ = false;
    mutable bool warned_ = false;
    mutable uint64_t hits_ = 0;
    mutable uint64_t misses_ = 0;
};

}
