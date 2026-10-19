#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Font.h"
#include "Geometry.h"

namespace ui {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    // 0xRRGGBB, sRGB
    static Color hex(uint32_t rgb, float alpha = 1.0f);
    // Hue 0-360, saturation and lightness 0-1
    static Color hsl(float hue, float saturation, float lightness, float alpha = 1.0f);
    // The other way: hue 0-360, saturation and lightness 0-1
    void toHsl(float &hue, float &saturation, float &lightness) const;
    uint32_t toHex() const;
};

// How a surface sits relative to the background. The order matches the shader's encoding.
enum class Elevation { Raised, Flat, Inset };
// Outline of a surface. Triangles point the named way and Cross is an X; those three are meant
// for flat fills (icons).
enum class Shape { Rounded, Soft, Pill, Circle, TriangleRight, TriangleLeft, TriangleDown, Cross, TriangleUp };
// Shading across a surface's face: Convex is lit like a dome, Concave like a bowl
enum class Finish { Flat, Convex, Concave };
// How far a component's surface stands out, within the theme's depth
enum class Depth { Subtle, Normal, Deep };
enum class Size { Small, Medium, Large };
// Horizontal placement of a line of text inside its box
enum class TextAlign { Start, Center, End };
// Typographic scale, each a font size relative to Body
enum class TextStyle { Caption, Body, Subtitle, Title, Headline, Display };
// Emphasis: how far text stands from the background. OnAccent is for text on an accent fill.
enum class TextTone { Primary, Secondary, Muted, Accent, OnAccent };

// A fully resolved neumorphic surface: everything the UI renderer needs to draw one
struct Surface {
    Rect rect;
    float radius = 0.0f;
    Elevation elevation = Elevation::Flat;
    float distance = 0.0f; // shadow offset in pixels
    float blur = 0.0f;
    Color base;
    float opacity = 1.0f;
    Color light;
    Color dark;
    float intensity = 0.0f;
    Vec2 lightDirection{-0.7071f, -0.7071f}; // unit vector toward the light, y down
    float curvature = 0.0f; // face shading: > 0 convex, < 0 concave
    float rim = 0.0f;       // a fine lit edge toward the light (raised) or away (inset), 0-1
    float form = 0.0f;      // outline in the shader: 0 rounded rect, 1 triangle right, 2 triangle left, 3 cross, 4 triangle down
};

// A fully resolved line of text, the drawable primitive alongside Surface
struct Text {
    std::string text;
    Vec2 baseline; // left end of the baseline, pixels
    float size = 0.0f; // em size, pixels
    Color color;
};

// A straight stroke with round caps, the third drawable primitive: from `a` to `b`, `width`
// pixels thick (at least about 1 to read), anti-aliased like surfaces' edges
struct Line {
    Vec2 a;
    Vec2 b;
    float width = 1.0f;
    Color color;
    float opacity = 1.0f;
};

// How a Plot draws its values
enum class PlotMode {
    Line,  // the values evenly spaced across the rect (the first at its left edge, the last at its
           // right), joined by a line `width` pixels thick
    Fill,  // the area between that curve and `baseline`
    Range, // `count` columns of a low and a high value (a waveform's min and max), each
           // rect.w / count wide, the bar `barFraction` of it, centred
};

// A curve, filled curve or waveform drawn from values in one primitive. The values are copied when
// it is added to a DrawList, so `values` only needs to live through that call.
struct Plot {
    Rect rect;
    PlotMode mode = PlotMode::Line;
    const float *values = nullptr; // `count` values; Range: `count` pairs (low, high), 2 x count floats
    uint32_t count = 0;
    float low = -1.0f;      // the value at the rect's bottom
    float high = 1.0f;      // the value at the rect's top
    float baseline = 0.0f;  // Fill: filled between the curve and this value
    float width = 2.0f;     // Line: the stroke's width in pixels
    float barFraction = 1.0f; // Range: the share of each column its bar fills
    Color color;
    float opacity = 1.0f;
};

// Styling layer. The only place visual values live, and it is strictly neumorphic:
// - every surface shares the background colour; depth comes from shadows alone
// - shadow and text colours are derived from the base colour, never set directly
// - each customisation is clamped to the range where the effect still reads
class Theme {
public:
    // Presets. Each is a base and accent colour pair; everything else starts at the defaults.
    static Theme light();
    static Theme dark();
    static Theme fromColors(Color base, Color accent);
    static std::vector<std::string> presetNames(); // "Light", "Dark", "Ocean", ...
    static Theme preset(const std::string &name);  // unknown names give light()

    // Base colour of background and surfaces. Saturation is capped and lightness is pulled into
    // the light (0.80-0.94) or dark (0.10-0.30) band, whichever is closer.
    Theme &setBaseColor(Color color);
    // Keeps the base hue and saturation and moves it into the light or dark band
    Theme &setDarkMode(bool dark);
    // Used sparingly for state (toggle on, slider fill, primary buttons)
    Theme &setAccentColor(Color color);
    Theme &setDepth(float pixels);        // shadow offset, 2-10
    Theme &setSoftness(float factor);     // blur = depth * softness, 1-3
    Theme &setCornerRadius(float px);     // 4-28
    Theme &setIntensity(float amount);    // shadow strength, 0.4-1
    Theme &setLightAngle(float degrees);  // where the light comes from, clockwise from 12 o'clock; 315 is top-left
    Theme &setCurvature(float amount);    // strength of Convex/Concave finishes, 0-0.6
    Theme &setTextContrast(float amount); // lightness gap between text and base, 0.35-0.6
    Theme &setFontScale(float scale);     // 0.85-1.25
    Theme &setSpacingScale(float scale);  // multiplies the minimum gaps and padding, 1-2
    Theme &setRim(float amount);          // the fine lit edge on raised and inset surfaces, 0-1
    Theme &setControlScale(float scale);  // control heights: 0.8 compact to 1.2 roomy

    // A hash of every value the theme holds (colours, every setting) and its font, so caches can
    // tell when the look changed
    uint64_t fingerprint() const;

    Color baseColor() const { return base_; }
    Color accentColor() const { return accent_; }
    Color lightShadow() const;
    Color darkShadow() const;
    bool isDark() const;

    float depth() const { return depth_; }
    float softness() const { return softness_; }
    float blur() const { return depth_ * softness_; }
    float cornerRadius() const { return radius_; }
    float intensity() const { return intensity_; }
    float lightAngle() const { return lightAngle_; }
    float curvature() const { return curvature_; }
    float textContrast() const { return textContrast_; }
    float fontScale() const { return fontScale_; }
    float spacingScale() const { return spacingScale_; }
    float rim() const { return rim_; }
    float controlScale() const { return controlScale_; }
    Vec2 lightDirection() const;

    // Layout rules derived from the shadow size, so neighbouring shadows don't collide
    float shadowExtent() const { return depth_ + blur(); }
    float spacing() const { return 1.5f * shadowExtent() * spacingScale_; } // minimum gap between siblings
    float padding() const { return 1.5f * shadowExtent() * spacingScale_; } // minimum inset inside panels

    float controlHeight(Size size) const;
    float radiusFor(Shape shape, const Rect &rect) const;
    float depthScale(Depth depth) const;

    // `depthScale` lets components lift (hover) or soften (small parts) a surface within the theme
    Surface surface(const Rect &rect, Shape shape, Elevation elevation, float depthScale = 1.0f,
                    Finish finish = Finish::Flat) const;
    // Flat accent-coloured fill, for indicators drawn on top of surfaces
    Surface accent(const Rect &rect, Shape shape, float opacity = 1.0f) const;
    // Flat fill in a text colour, for icon strokes, and faint (opacity below 1) for grid lines
    Surface ink(const Rect &rect, Shape shape, TextTone tone = TextTone::Primary, bool enabled = true,
                float opacity = 1.0f) const;
    // Translucent base-coloured veil that pushes everything under it back (behind drawers, dialogs)
    Surface scrim(const Rect &rect, float opacity) const;
    // Strokes in a text colour (`tone`) or the accent, for curves, guides and graphs
    Line inkLine(Vec2 a, Vec2 b, float width, TextTone tone = TextTone::Primary, float opacity = 1.0f) const;
    Line accentLine(Vec2 a, Vec2 b, float width, float opacity = 1.0f) const;
    // A plot in a text colour or the accent (colour and opacity set as ink and accent do); the
    // caller sets values, count, low, high and the mode's fields
    Plot inkPlot(const Rect &rect, PlotMode mode, TextTone tone = TextTone::Secondary, float opacity = 1.0f) const;
    Plot accentPlot(const Rect &rect, PlotMode mode, float opacity = 1.0f) const;

    // Typography. Text is the base hue pushed toward the shadow side (darker on light themes,
    // lighter on dark ones): low contrast, as neumorphism expects, but readable.
    Color textColor(TextTone tone = TextTone::Primary) const;
    Color mutedTextColor() const; // disabled components
    // Font sizes are scaled by the font scale and snapped to a size baked into the atlas
    float fontSize(Size size) const;
    float fontSize(TextStyle style) const;
    const Font &font() const { return *font_; }
    // Width of `text` and the height of its line
    Vec2 measureText(const std::string &text, float px) const;
    Vec2 measureText(const std::string &text, Size size) const;
    Vec2 measureText(const std::string &text, TextStyle style) const;
    // `text` centred vertically in `box` (by cap height) and placed horizontally by `align`
    Text text(const std::string &text, const Rect &box, float px, TextAlign align, Color color) const;
    Text text(const std::string &text, const Rect &box, Size size, TextAlign align = TextAlign::Center,
              bool enabled = true) const;
    Text text(const std::string &text, const Rect &box, TextStyle style, TextTone tone = TextTone::Primary,
              TextAlign align = TextAlign::Start) const;

private:
    Theme();

    std::shared_ptr<const Font> font_; // shared by every theme, so swapping themes keeps the atlas
    Color base_;
    Color accent_;
    float depth_ = 6.0f;
    float softness_ = 2.0f;
    float radius_ = 14.0f;
    float intensity_ = 0.9f;
    float lightAngle_ = 315.0f;
    float curvature_ = 0.3f;
    float textContrast_ = 0.5f;
    float fontScale_ = 1.0f;
    float spacingScale_ = 1.0f;
    float rim_ = 0.0f;
    float controlScale_ = 1.0f;
};

}
