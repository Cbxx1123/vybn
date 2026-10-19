#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Geometry.h"

namespace ui {

// One glyph ready to draw: a pixel quad and the matching region of the atlas
struct GlyphQuad {
    float x0, y0, x1, y1; // pixels, y down
    float u0, v0, u1, v1; // normalised atlas coordinates
};

// A TrueType font baked with stb_truetype into one single-channel coverage atlas, at a fixed set
// of pixel sizes (em size, like CSS px). Covers printable ASCII; anything else draws as '?'.
// CPU-side only: UIRenderer uploads atlas() to the GPU. Not copyable.
class Font {
public:
    Font(const std::string &path, const std::vector<float> &pixelSizes);
    ~Font();

    Font(const Font &) = delete;
    Font &operator=(const Font &) = delete;

    // The bundled font (assets/fonts/Inter-Medium.ttf), or Segoe UI if the bundled file is missing
    static std::string defaultPath();

    // The baked size nearest to `size`; drawing at baked sizes keeps glyphs crisp
    float snap(float size) const;

    // Metrics at `size` px. Sizes that weren't baked are scaled from the nearest baked one.
    float measure(const std::string &text, float size) const; // advance width
    float ascent(float size) const;
    float descent(float size) const; // negative: below the baseline
    float lineHeight(float size) const;
    float capHeight(float size) const;

    // Appends one quad per visible glyph of `text`, starting at `baseline` (left end of the baseline)
    void layout(const std::string &text, Vec2 baseline, float size, std::vector<GlyphQuad> &out) const;

    uint32_t atlasWidth() const { return atlasWidth_; }
    uint32_t atlasHeight() const { return atlasHeight_; }
    const std::vector<uint8_t> &atlas() const { return atlas_; }

private:
    struct Impl; // stb_truetype state, kept out of this header
    struct Face;

    const Face &faceFor(float size) const;

    std::unique_ptr<Impl> impl_;
    uint32_t atlasWidth_ = 0;
    uint32_t atlasHeight_ = 0;
    std::vector<uint8_t> atlas_;
};

}
