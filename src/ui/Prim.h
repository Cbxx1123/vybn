#pragma once

#include <cstdint>
#include <vector>

#include "Font.h"
#include "Geometry.h"
#include "Image.h"
#include "Theme.h"

namespace ui {

// A live image drawn into a rect (pixel for pixel when the rect matches its size)
struct ImageDraw {
    Rect rect;
    const ImageData *image = nullptr; // must outlive the frame's rendering
    float opacity = 1.0f;
};

// What a Prim draws. The values are the shader's encoding (shaders/prim.frag).
enum class PrimKind : uint32_t { Surface = 0, Glyph = 1, Image = 2, Line = 3, Plot = 4 };

// The clip of a prim drawn with no clip: the whole plane
constexpr float kNoClip = 1e7f;

// A colour packed as the GPU takes it: red in the low byte, then green, blue and alpha (what
// GLSL's unpackUnorm4x8 reads back), each component clamped to 0-1 and rounded to 1/255
inline uint32_t packColor(float r, float g, float b, float a)
{
    auto byte = [](float v) {
        v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (byte(a) << 24);
}
inline Color unpackColor(uint32_t c)
{
    return {static_cast<float>(c & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
            static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>(c >> 24) / 255.0f};
}

// One thing drawn, as the GPU takes it: an instance of shaders/prim.vert. Members are inputs at
// the location of their order: 0-3 are vec4s (R32G32B32A32_SFLOAT), 4 is a uvec4
// (R32G32B32A32_UINT) of base, light, dark and kind. Six vertices make its quad.
struct Prim {
    float rect[4];  // 0: x, y, w, h in framebuffer pixels
    float clip[4];  // 1: x0, y0, x1, y1 in whole pixels; nothing outside is drawn
    float shape[4]; // 2: Surface: radius, shadow distance, blur, elevation (0 raised, 1 flat,
                    //    2 inset) + rim * 0.45. Glyph, Image: u0, v0, u1, v1. Line: a.x, a.y,
                    //    b.x, b.y (framebuffer pixels). Plot: the index of its first value in
                    //    the plot data, its count (pairs for Range), low, high
    float style[4]; // 3: Surface: light direction x, y (unit, y down), curvature, form (0 rounded
                    //    rect, 1 triangle right, 2 triangle left, 3 cross, 4 triangle down, 5 up).
                    //    Line: half the width, 0, 0, 0. Plot: mode (0 Line, 1 Fill, 2 Range),
                    //    half the width (Line) or barFraction (Range), baseline (Fill), 0
    // 4: packed colours (packColor), sRGB:
    uint32_t base;  //    a surface's colour and opacity, a glyph's colour and alpha, an image's
                    //    opacity in alpha (rgb white)
    uint32_t light; //    Surface: light shadow rgb, intensity in alpha
    uint32_t dark;  //    Surface: dark shadow rgb, intensity in alpha
    uint32_t kind;  //    PrimKind
};
static_assert(sizeof(Prim) == 80, "Prim must match shaders/prim.vert's inputs");

// The clip prims carry: `clip` rounded out to whole pixels (floor of its start, ceil of its end,
// as the scissor used to be), or kNoClip on every side when `clipped` is false
void clipBounds(const Rect &clip, bool clipped, float out[4]);

// A surface as a prim, packed as the old UIRenderer's toInstance did: the rim's strength in the
// fraction of the elevation (elevation + clamp(rim, 0, 1) * 0.45), the form in style[3]
Prim surfacePrim(const Surface &surface, const float clip[4]);
// One Glyph prim per visible glyph of `text`, from Font::layout: rect x0, y0, x1 - x0, y1 - y0; uv
// in shape; the text's colour in base. Appended to `out`.
void glyphPrims(const Font &font, const Text &text, const float clip[4], std::vector<Prim> &out);
// A line: rect = the capsule's bounds (both ends, grown by half the width plus 1 px for the
// anti-aliased edge), shape = the ends, style[0] = half the width (at least 0.5), base = its colour
// with the opacity as alpha
Prim linePrim(const Line &line, const float clip[4]);
// A plot whose values start at `first` in the plot data: rect = the plot's rect (prim.vert grows a
// Line plot's quad by half its width + 1, as a raised surface's by its shadow), shape and style as
// above, base = its colour with the opacity as alpha. Index and count are floats, exact below 2^24.
Prim plotPrim(const Plot &plot, uint32_t first, const float clip[4]);
// An image's quad: its rect, uv 0 to 1, base white with the opacity as its alpha
Prim imagePrim(const ImageDraw &image, const float clip[4]);

}
