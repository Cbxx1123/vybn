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

// What a Prim draws. The values are the shader's encoding (shaders/prim.frag); Line and Plot are
// for lines and plots (refactor 2) and not drawn yet.
enum class PrimKind : uint32_t { Surface = 0, Glyph = 1, Image = 2, Line = 3, Plot = 4 };

// The clip of a prim drawn with no clip: the whole plane
constexpr float kNoClip = 1e7f;

// One thing drawn, as the GPU takes it: an instance of shaders/prim.vert. Every member is an
// input at the location of its order: 0-6 are vec4s (R32G32B32A32_SFLOAT), 7 the kind (R32_UINT).
// Six vertices make its quad.
struct Prim {
    float rect[4];  // 0: x, y, w, h in framebuffer pixels
    float clip[4];  // 1: x0, y0, x1, y1 in whole pixels; nothing outside is drawn
    float shape[4]; // 2: Surface: radius, shadow distance, blur, elevation (0 raised, 1 flat,
                    //    2 inset) + rim * 0.45. Glyph, Image: u0, v0, u1, v1
    float base[4];  // 3: sRGB rgb and opacity: a surface's colour, a glyph's colour and alpha, an
                    //    image's opacity in [3]
    float light[4]; // 4: Surface: light shadow sRGB rgb, intensity
    float dark[4];  // 5: Surface: dark shadow sRGB rgb, intensity
    float style[4]; // 6: Surface: light direction x, y (unit, y down), curvature, form (0 rounded
                    //    rect, 1 triangle right, 2 triangle left, 3 cross, 4 triangle down, 5 up)
    uint32_t kind;  // 7: PrimKind
    uint32_t pad[3];
};
static_assert(sizeof(Prim) == 128, "Prim must match shaders/prim.vert's inputs");

// The clip prims carry: `clip` rounded out to whole pixels (floor of its start, ceil of its end,
// as the scissor used to be), or kNoClip on every side when `clipped` is false
void clipBounds(const Rect &clip, bool clipped, float out[4]);

// A surface as a prim, packed as the old UIRenderer's toInstance did: the rim's strength in the
// fraction of the elevation (elevation + clamp(rim, 0, 1) * 0.45), the form in style[3]
Prim surfacePrim(const Surface &surface, const float clip[4]);
// One Glyph prim per visible glyph of `text`, from Font::layout: rect x0, y0, x1 - x0, y1 - y0; uv
// in shape; the text's colour in base. Appended to `out`.
void glyphPrims(const Font &font, const Text &text, const float clip[4], std::vector<Prim> &out);
// An image's quad: its rect, uv 0 to 1, opacity in base[3] (base rgb 1)
Prim imagePrim(const ImageDraw &image, const float clip[4]);

}
