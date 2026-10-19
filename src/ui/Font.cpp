#include "Font.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#ifndef ENGINE_ASSET_DIR
#define ENGINE_ASSET_DIR "assets"
#endif

namespace ui {

namespace {

constexpr int kFirstChar = 32; // space
constexpr int kCharCount = 95; // through '~'
constexpr int kFallbackChar = '?';

// Oversampled glyphs stay sharp when the quad lands between pixels and sample well with
// bilinear filtering; 2x2 is the usual choice for small UI text. Large text needs less.
constexpr unsigned int kOversample = 2;
constexpr float kLargeText = 24.0f; // sizes from here up are baked with 1x1

int toCodepoint(unsigned char c)
{
    return c >= kFirstChar && c < kFirstChar + kCharCount ? c : kFallbackChar;
}

}

struct Font::Face {
    float size = 0.0f;  // em size in pixels
    float scale = 0.0f; // font units to pixels
    std::vector<stbtt_packedchar> chars;
};

struct Font::Impl {
    std::vector<unsigned char> data; // stbtt_fontinfo points into this
    stbtt_fontinfo info{};
    std::vector<Face> faces;
    // In font units
    int ascent = 0;
    int descent = 0;
    int lineGap = 0;
    int capHeight = 0;
};

Font::Font(const std::string &path, const std::vector<float> &pixelSizes)
    : impl_(std::make_unique<Impl>())
{
    if (pixelSizes.empty())
        throw std::runtime_error("Font needs at least one pixel size");

    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Failed to open font " + path);
    impl_->data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

    const unsigned char *data = impl_->data.data();
    const int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&impl_->info, data, offset))
        throw std::runtime_error("Invalid TrueType font " + path);

    stbtt_GetFontVMetrics(&impl_->info, &impl_->ascent, &impl_->descent, &impl_->lineGap);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    impl_->capHeight = stbtt_GetCodepointBox(&impl_->info, 'H', &x0, &y0, &x1, &y1) ? y1 : impl_->ascent;

    for (float size : pixelSizes) {
        Face face;
        face.size = size;
        face.scale = stbtt_ScaleForMappingEmToPixels(&impl_->info, size);
        face.chars.resize(kCharCount);
        impl_->faces.push_back(std::move(face));
    }

    // Small and large sizes are packed separately, since they use different oversampling
    std::vector<stbtt_pack_range> small;
    std::vector<stbtt_pack_range> large;
    for (Face &face : impl_->faces) {
        stbtt_pack_range range{};
        range.font_size = STBTT_POINT_SIZE(face.size); // em size, not ascent-to-descent
        range.first_unicode_codepoint_in_range = kFirstChar;
        range.num_chars = kCharCount;
        range.chardata_for_range = face.chars.data();
        (face.size < kLargeText ? small : large).push_back(range);
    }

    // Every size goes into one atlas; start small and double its area (height, then width) until
    // everything fits, so it is never more than twice the room the glyphs need
    for (uint32_t width = 256, height = 128; width <= 4096; (height < width ? height : width) *= 2) {
        atlas_.assign(static_cast<size_t>(width) * height, 0);
        stbtt_pack_context context;
        if (!stbtt_PackBegin(&context, atlas_.data(), static_cast<int>(width), static_cast<int>(height), 0, 1, nullptr))
            throw std::runtime_error("stbtt_PackBegin failed");
        int packed = 1;
        if (!small.empty()) {
            stbtt_PackSetOversampling(&context, kOversample, kOversample);
            packed = stbtt_PackFontRanges(&context, data, 0, small.data(), static_cast<int>(small.size()));
        }
        if (packed && !large.empty()) {
            stbtt_PackSetOversampling(&context, 1, 1);
            packed = stbtt_PackFontRanges(&context, data, 0, large.data(), static_cast<int>(large.size()));
        }
        stbtt_PackEnd(&context);
        if (packed) {
            atlasWidth_ = width;
            atlasHeight_ = height;
            atlas_.shrink_to_fit();
            return;
        }
    }
    throw std::runtime_error("Glyphs of " + path + " don't fit in a 4096x4096 atlas");
}

Font::~Font() = default;

std::string Font::defaultPath()
{
    const std::string bundled = ENGINE_ASSET_DIR "/fonts/Inter-Medium.ttf";
    if (std::ifstream(bundled).good())
        return bundled;
    return "C:/Windows/Fonts/segoeui.ttf";
}

const Font::Face &Font::faceFor(float size) const
{
    const Face *best = &impl_->faces.front();
    for (const Face &face : impl_->faces) {
        if (std::fabs(face.size - size) < std::fabs(best->size - size))
            best = &face;
    }
    return *best;
}

float Font::snap(float size) const
{
    return faceFor(size).size;
}

float Font::measure(const std::string &text, float size) const
{
    const Face &face = faceFor(size);
    float width = 0.0f;
    int previous = 0;
    for (unsigned char c : text) {
        const int codepoint = toCodepoint(c);
        if (previous)
            width += stbtt_GetCodepointKernAdvance(&impl_->info, previous, codepoint) * face.scale;
        width += face.chars[codepoint - kFirstChar].xadvance;
        previous = codepoint;
    }
    return width * size / face.size;
}

float Font::ascent(float size) const
{
    return impl_->ascent * stbtt_ScaleForMappingEmToPixels(&impl_->info, size);
}

float Font::descent(float size) const
{
    return impl_->descent * stbtt_ScaleForMappingEmToPixels(&impl_->info, size);
}

float Font::lineHeight(float size) const
{
    return (impl_->ascent - impl_->descent + impl_->lineGap) * stbtt_ScaleForMappingEmToPixels(&impl_->info, size);
}

float Font::capHeight(float size) const
{
    return impl_->capHeight * stbtt_ScaleForMappingEmToPixels(&impl_->info, size);
}

void Font::layout(const std::string &text, Vec2 baseline, float size, std::vector<GlyphQuad> &out) const
{
    const Face &face = faceFor(size);
    const float k = size / face.size;

    // stb advances the pen in the baked face's pixels, relative to the baseline
    float x = 0.0f;
    float y = 0.0f;
    int previous = 0;
    for (unsigned char c : text) {
        const int codepoint = toCodepoint(c);
        if (previous)
            x += stbtt_GetCodepointKernAdvance(&impl_->info, previous, codepoint) * face.scale;

        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(face.chars.data(), static_cast<int>(atlasWidth_), static_cast<int>(atlasHeight_),
                            codepoint - kFirstChar, &x, &y, &q, 0);
        if (q.x1 > q.x0 && q.y1 > q.y0) {
            out.push_back({baseline.x + q.x0 * k, baseline.y + q.y0 * k, baseline.x + q.x1 * k, baseline.y + q.y1 * k,
                           q.s0, q.t0, q.s1, q.t1});
        }
        previous = codepoint;
    }
}

}
