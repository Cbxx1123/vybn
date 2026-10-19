#include "Theme.h"

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

struct Hsl {
    float h, s, l;
};

Hsl toHsl(const Color &c)
{
    float hi = std::max({c.r, c.g, c.b});
    float lo = std::min({c.r, c.g, c.b});
    float l = (hi + lo) * 0.5f;
    float d = hi - lo;
    if (d < 1e-6f)
        return {0.0f, 0.0f, l};

    float s = l > 0.5f ? d / (2.0f - hi - lo) : d / (hi + lo);
    float h;
    if (hi == c.r)
        h = (c.g - c.b) / d + (c.g < c.b ? 6.0f : 0.0f);
    else if (hi == c.g)
        h = (c.b - c.r) / d + 2.0f;
    else
        h = (c.r - c.g) / d + 4.0f;
    return {h / 6.0f, s, l};
}

float hueToChannel(float p, float q, float t)
{
    if (t < 0.0f)
        t += 1.0f;
    if (t > 1.0f)
        t -= 1.0f;
    if (t < 1.0f / 6.0f)
        return p + (q - p) * 6.0f * t;
    if (t < 0.5f)
        return q;
    if (t < 2.0f / 3.0f)
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

Color fromHsl(const Hsl &hsl, float alpha)
{
    if (hsl.s <= 0.0f)
        return {hsl.l, hsl.l, hsl.l, alpha};
    float q = hsl.l < 0.5f ? hsl.l * (1.0f + hsl.s) : hsl.l + hsl.s - hsl.l * hsl.s;
    float p = 2.0f * hsl.l - q;
    return {hueToChannel(p, q, hsl.h + 1.0f / 3.0f), hueToChannel(p, q, hsl.h), hueToChannel(p, q, hsl.h - 1.0f / 3.0f), alpha};
}

Color mix(const Color &a, const Color &b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a};
}

Color scale(const Color &c, float k)
{
    return {c.r * k, c.g * k, c.b * k, c.a};
}

const Color white{1.0f, 1.0f, 1.0f, 1.0f};

// Every font size the atlas holds. Theme sizes snap to these so text stays crisp.
const std::vector<float> &bakedSizes()
{
    static const std::vector<float> sizes{11, 12, 13, 14, 15, 16, 17, 18, 20, 22, 24, 26, 28, 30, 32, 36, 40, 44, 48, 56};
    return sizes;
}

// Loaded once on first use and shared by every Theme
std::shared_ptr<const Font> sharedFont()
{
    static const std::shared_ptr<const Font> font = std::make_shared<const Font>(Font::defaultPath(), bakedSizes());
    return font;
}

struct Preset {
    const char *name;
    uint32_t base;
    uint32_t accent;
};

const Preset kPresets[] = {
    {"Light", 0xE0E5EC, 0x6C8EF5},
    {"Dark", 0x2C3038, 0x7AA2FF},
    {"Ocean", 0xDCE8F0, 0x2F9BD6},
    {"Sand", 0xEDE6DC, 0xE08A4C},
    {"Mint", 0xDDEDE6, 0x2FB58A},
    {"Rose", 0xF0E2E6, 0xE0607E},
    {"Lavender", 0xE6E2F2, 0x8C6FE0},
    {"Graphite", 0x2B2B2E, 0xF0B04A},
    {"Midnight", 0x1F2638, 0x5AD1C8},
    {"Carbon", 0x1F2227, 0x7C9CFF},
    {"Obsidian", 0x1D1C23, 0xA48CFF},
    {"Slate", 0x252A31, 0x4FC3B8},
    {"Ember", 0x25211F, 0xF08A4B},
};

}

Color Color::hex(uint32_t rgb, float alpha)
{
    return {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}

Color Color::hsl(float hue, float saturation, float lightness, float alpha)
{
    float h = std::fmod(hue, 360.0f);
    if (h < 0.0f)
        h += 360.0f;
    return fromHsl({h / 360.0f, std::clamp(saturation, 0.0f, 1.0f), std::clamp(lightness, 0.0f, 1.0f)}, alpha);
}

void Color::toHsl(float &hue, float &saturation, float &lightness) const
{
    const Hsl h = ::ui::toHsl(*this);
    hue = h.h * 360.0f;
    saturation = h.s;
    lightness = h.l;
}

uint32_t Color::toHex() const
{
    auto channel = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

Theme::Theme()
    : font_(sharedFont())
{
}

Theme Theme::light()
{
    return preset("Light");
}

Theme Theme::dark()
{
    return preset("Dark");
}

Theme Theme::fromColors(Color base, Color accent)
{
    Theme theme;
    theme.setBaseColor(base);
    theme.setAccentColor(accent);
    return theme;
}

std::vector<std::string> Theme::presetNames()
{
    std::vector<std::string> names;
    for (const Preset &preset : kPresets)
        names.emplace_back(preset.name);
    return names;
}

Theme Theme::preset(const std::string &name)
{
    for (const Preset &preset : kPresets) {
        if (name == preset.name)
            return fromColors(Color::hex(preset.base), Color::hex(preset.accent));
    }
    return fromColors(Color::hex(kPresets[0].base), Color::hex(kPresets[0].accent));
}

Theme &Theme::setBaseColor(Color color)
{
    // Neumorphism needs a low-saturation mid-light or mid-dark base: pure white has no room
    // for a highlight, pure black none for a shadow, and strong hues make the shadows muddy.
    Hsl hsl = toHsl(color);
    hsl.s = std::min(hsl.s, 0.25f);
    hsl.l = hsl.l >= 0.5f ? std::clamp(hsl.l, 0.80f, 0.94f) : std::clamp(hsl.l, 0.10f, 0.30f);
    base_ = fromHsl(hsl, 1.0f);
    return *this;
}

Theme &Theme::setDarkMode(bool dark)
{
    if (dark == isDark())
        return *this;
    // Mirror the lightness across the middle, so a pale base becomes a deep one of the same hue
    Hsl hsl = toHsl(base_);
    hsl.l = dark ? std::clamp(1.04f - hsl.l, 0.12f, 0.22f) : std::clamp(1.04f - hsl.l, 0.84f, 0.92f);
    base_ = fromHsl(hsl, 1.0f);
    return *this;
}

Theme &Theme::setAccentColor(Color color)
{
    // Any hue, but kept soft enough to sit on a neumorphic surface
    Hsl hsl = toHsl(color);
    hsl.s = std::clamp(hsl.s, 0.35f, 0.80f);
    hsl.l = std::clamp(hsl.l, 0.45f, 0.70f);
    accent_ = fromHsl(hsl, 1.0f);
    return *this;
}

Theme &Theme::setDepth(float pixels)
{
    depth_ = std::clamp(pixels, 2.0f, 10.0f);
    return *this;
}

Theme &Theme::setSoftness(float factor)
{
    softness_ = std::clamp(factor, 1.0f, 3.0f);
    return *this;
}

Theme &Theme::setCornerRadius(float px)
{
    radius_ = std::clamp(px, 4.0f, 28.0f);
    return *this;
}

Theme &Theme::setIntensity(float amount)
{
    intensity_ = std::clamp(amount, 0.4f, 1.0f);
    return *this;
}

Theme &Theme::setLightAngle(float degrees)
{
    lightAngle_ = std::fmod(degrees, 360.0f);
    if (lightAngle_ < 0.0f)
        lightAngle_ += 360.0f;
    return *this;
}

Theme &Theme::setCurvature(float amount)
{
    curvature_ = std::clamp(amount, 0.0f, 0.6f);
    return *this;
}

Theme &Theme::setTextContrast(float amount)
{
    textContrast_ = std::clamp(amount, 0.35f, 0.6f);
    return *this;
}

Theme &Theme::setFontScale(float scale)
{
    fontScale_ = std::clamp(scale, 0.85f, 1.25f);
    return *this;
}

Theme &Theme::setSpacingScale(float scale)
{
    spacingScale_ = std::clamp(scale, 1.0f, 2.0f);
    return *this;
}

Theme &Theme::setRim(float amount)
{
    rim_ = std::clamp(amount, 0.0f, 1.0f);
    return *this;
}

Theme &Theme::setControlScale(float scale)
{
    controlScale_ = std::clamp(scale, 0.8f, 1.2f);
    return *this;
}

bool Theme::isDark() const
{
    return toHsl(base_).l < 0.5f;
}

Color Theme::lightShadow() const
{
    return isDark() ? mix(base_, white, 0.085f) : mix(base_, white, 0.75f);
}

Color Theme::darkShadow() const
{
    return isDark() ? scale(base_, 0.46f) : scale(base_, 0.72f);
}

Vec2 Theme::lightDirection() const
{
    const float radians = lightAngle_ * 3.14159265f / 180.0f;
    return {std::sin(radians), -std::cos(radians)};
}

float Theme::controlHeight(Size size) const
{
    float px = 48.0f;
    if (size == Size::Small)
        px = 36.0f;
    else if (size == Size::Large)
        px = 64.0f;
    return std::round(px * controlScale_);
}

float Theme::radiusFor(Shape shape, const Rect &rect) const
{
    float half = std::min(rect.w, rect.h) * 0.5f;
    switch (shape) {
    case Shape::Rounded:
        return std::min(radius_, half);
    case Shape::Soft:
        return std::min(radius_ * 0.45f, half);
    case Shape::TriangleRight:
    case Shape::TriangleLeft:
    case Shape::TriangleDown:
    case Shape::TriangleUp:
        return std::min(1.5f, half); // just softens the corners
    case Shape::Cross:
        return std::max(1.0f, half * 0.13f); // half the stroke width
    case Shape::Pill:
    case Shape::Circle:
    default:
        return half;
    }
}

float Theme::depthScale(Depth depth) const
{
    switch (depth) {
    case Depth::Subtle:
        return 0.55f;
    case Depth::Deep:
        return 1.5f;
    case Depth::Normal:
    default:
        return 1.0f;
    }
}

namespace {

float formFor(Shape shape)
{
    if (shape == Shape::TriangleRight)
        return 1.0f;
    if (shape == Shape::TriangleLeft)
        return 2.0f;
    if (shape == Shape::Cross)
        return 3.0f;
    if (shape == Shape::TriangleDown)
        return 4.0f;
    if (shape == Shape::TriangleUp)
        return 5.0f;
    return 0.0f;
}

}

Surface Theme::surface(const Rect &rect, Shape shape, Elevation elevation, float depthScale, Finish finish) const
{
    Surface s;
    s.rect = rect;
    s.radius = radiusFor(shape, rect);
    s.elevation = elevation;
    s.distance = depth_ * depthScale;
    s.blur = blur() * depthScale;
    s.base = base_;
    s.light = lightShadow();
    s.dark = darkShadow();
    s.intensity = intensity_;
    s.lightDirection = lightDirection();
    s.curvature = finish == Finish::Convex ? curvature_ : finish == Finish::Concave ? -curvature_ : 0.0f;
    s.form = formFor(shape);
    // A rim only on the rounded outlines, where it reads as an edge
    s.rim = elevation == Elevation::Flat || s.form != 0.0f ? 0.0f : rim_ * std::min(1.0f, depthScale + 0.3f);
    return s;
}

Surface Theme::accent(const Rect &rect, Shape shape, float opacity) const
{
    Surface s;
    s.rect = rect;
    s.radius = radiusFor(shape, rect);
    s.elevation = Elevation::Flat;
    s.base = accent_;
    s.opacity = opacity;
    s.form = formFor(shape);
    return s;
}

Surface Theme::ink(const Rect &rect, Shape shape, TextTone tone, bool enabled, float opacity) const
{
    Surface s = accent(rect, shape, std::clamp(opacity, 0.0f, 1.0f));
    s.base = enabled ? textColor(tone) : mutedTextColor();
    return s;
}

Line Theme::inkLine(Vec2 a, Vec2 b, float width, TextTone tone, float opacity) const
{
    Line line;
    line.a = a;
    line.b = b;
    line.width = width;
    line.color = textColor(tone);
    line.opacity = std::clamp(opacity, 0.0f, 1.0f);
    return line;
}

Line Theme::accentLine(Vec2 a, Vec2 b, float width, float opacity) const
{
    Line line;
    line.a = a;
    line.b = b;
    line.width = width;
    line.color = accent_;
    line.opacity = std::clamp(opacity, 0.0f, 1.0f);
    return line;
}

Surface Theme::scrim(const Rect &rect, float opacity) const
{
    Surface s;
    s.rect = rect;
    s.elevation = Elevation::Flat;
    s.base = base_;
    s.opacity = std::clamp(opacity, 0.0f, 1.0f);
    return s;
}

Color Theme::textColor(TextTone tone) const
{
    if (tone == TextTone::Accent)
        return accent_;
    if (tone == TextTone::OnAccent)
        return mix(base_, white, isDark() ? 0.85f : 0.8f);

    // Same hue as the surface, a contrast-sized step of lightness away from it
    Hsl hsl = toHsl(base_);
    hsl.s = std::min(hsl.s * 1.5f, 0.25f);
    hsl.l = isDark() ? std::min(hsl.l + textContrast_, 0.82f) : std::max(hsl.l - textContrast_, 0.28f);
    const Color primary = fromHsl(hsl, 1.0f);
    if (tone == TextTone::Secondary)
        return mix(primary, base_, 0.28f);
    if (tone == TextTone::Muted)
        return mix(primary, base_, 0.45f);
    return primary;
}

Color Theme::mutedTextColor() const
{
    return mix(textColor(), base_, 0.5f);
}

float Theme::fontSize(Size size) const
{
    float px = 15.0f;
    if (size == Size::Small)
        px = 13.0f;
    else if (size == Size::Large)
        px = 18.0f;
    return font_->snap(px * fontScale_);
}

float Theme::fontSize(TextStyle style) const
{
    float px = 15.0f;
    switch (style) {
    case TextStyle::Caption:
        px = 12.0f;
        break;
    case TextStyle::Subtitle:
        px = 17.0f;
        break;
    case TextStyle::Title:
        px = 22.0f;
        break;
    case TextStyle::Headline:
        px = 30.0f;
        break;
    case TextStyle::Display:
        px = 44.0f;
        break;
    case TextStyle::Body:
    default:
        break;
    }
    return font_->snap(px * fontScale_);
}

Vec2 Theme::measureText(const std::string &text, float px) const
{
    return {font_->measure(text, px), font_->lineHeight(px)};
}

Vec2 Theme::measureText(const std::string &text, Size size) const
{
    return measureText(text, fontSize(size));
}

Vec2 Theme::measureText(const std::string &text, TextStyle style) const
{
    return measureText(text, fontSize(style));
}

Text Theme::text(const std::string &text, const Rect &box, float px, TextAlign align, Color color) const
{
    const float width = font_->measure(text, px);

    float x = box.x;
    if (align == TextAlign::Center)
        x += (box.w - width) * 0.5f;
    else if (align == TextAlign::End)
        x += box.w - width;

    // Centre capitals on the box's middle; whole pixels keep the baseline crisp
    const float baseline = box.y + (box.h + font_->capHeight(px)) * 0.5f;

    Text t;
    t.text = text;
    t.baseline = {std::round(x), std::round(baseline)};
    t.size = px;
    t.color = color;
    return t;
}

Text Theme::text(const std::string &text, const Rect &box, Size size, TextAlign align, bool enabled) const
{
    return this->text(text, box, fontSize(size), align, enabled ? textColor() : mutedTextColor());
}

Text Theme::text(const std::string &text, const Rect &box, TextStyle style, TextTone tone, TextAlign align) const
{
    return this->text(text, box, fontSize(style), align, textColor(tone));
}

}
