#include "daw/app/Appearance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

#include "daw/widgets/Format.h"
#include "daw/app/Settings.h"
#include "daw/io/Toml.h"
#include "daw/widgets/Widgets.h"

namespace daw {

using namespace ui;
namespace fs = std::filesystem;

namespace {

std::string hexText(Color c)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "#%06X", static_cast<unsigned>(c.toHex()));
    return buffer;
}

bool parseHex(const std::string &text, Color &out)
{
    std::string t = text;
    if (!t.empty() && t[0] == '#')
        t.erase(0, 1);
    if (t.size() != 6 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        return false;
    out = Color::hex(static_cast<uint32_t>(std::stoul(t, nullptr, 16)));
    return true;
}

// The lightness band a base colour is kept in, dark or light
void band(bool dark, float &low, float &high)
{
    low = dark ? 0.10f : 0.80f;
    high = dark ? 0.30f : 0.94f;
}

// A preset as a sample: its base with its accent on it, the name under it. Pressed in when chosen.
class Swatch : public Component {
public:
    Swatch(std::string name, std::function<bool()> chosen, std::function<void()> onClick)
        : name_(std::move(name)), theme_(Theme::preset(name_)), chosen_(std::move(chosen)), onClick_(std::move(onClick))
    {
    }

    Vec2 measure(const Theme &theme) const override { return {84.0f, 58.0f + theme.fontSize(TextStyle::Caption)}; }

    void handleInput(const Input &input) override
    {
        if (click_.update(rect_, input, enabled_) && onClick_)
            onClick_();
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        const bool chosen = chosen_ && chosen_();
        const Rect card{rect_.x, rect_.y, rect_.w, rect_.h};
        list.add(theme.surface(card, Shape::Rounded, chosen ? Elevation::Inset : Elevation::Raised,
                               chosen ? 0.6f : click_.hovered ? 0.8f : 0.55f));
        // The sample: the preset's own colours, which is what a swatch is for
        const Rect sample{card.x + 10.0f, card.y + 9.0f, card.w - 20.0f, 30.0f};
        Surface base = theme.accent(sample, Shape::Soft);
        base.base = theme_.baseColor();
        list.add(base);
        Surface dot = theme.accent({sample.x + sample.w - 22.0f, sample.y + 8.0f, 14.0f, 14.0f}, Shape::Circle);
        dot.base = theme_.accentColor();
        list.add(dot);
        Surface line = theme.accent({sample.x + 8.0f, sample.y + 13.0f, sample.w * 0.42f, 4.0f}, Shape::Pill);
        line.base = theme_.textColor(TextTone::Secondary);
        list.add(line);
        list.add(theme.text(name_, {card.x, sample.y + sample.h + 4.0f, card.w, card.y + card.h - sample.y - sample.h - 6.0f},
                            TextStyle::Caption, chosen ? TextTone::Accent : TextTone::Secondary, TextAlign::Center));
    }

private:
    std::string name_;
    Theme theme_;
    std::function<bool()> chosen_;
    std::function<void()> onClick_;
    ClickState click_;
};

// An accent colour to pick: a dot of it, ringed when it is the one in use
class ColorDot : public Component {
public:
    ColorDot(Color color, std::function<bool()> chosen, std::function<void()> onClick)
        : color_(color), chosen_(std::move(chosen)), onClick_(std::move(onClick))
    {
    }

    Vec2 measure(const Theme &) const override { return {30.0f, 30.0f}; }

    void handleInput(const Input &input) override
    {
        if (click_.update(rect_, input, enabled_) && onClick_)
            onClick_();
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        const bool chosen = chosen_ && chosen_();
        list.add(theme.surface(rect_, Shape::Circle, chosen ? Elevation::Inset : Elevation::Raised,
                               click_.hovered ? 0.6f : 0.4f));
        const float inset = chosen ? 8.0f : 6.0f;
        Surface dot = theme.accent({rect_.x + inset, rect_.y + inset, rect_.w - 2 * inset, rect_.h - 2 * inset}, Shape::Circle);
        dot.base = color_;
        list.add(dot);
    }

private:
    Color color_;
    std::function<bool()> chosen_;
    std::function<void()> onClick_;
    ClickState click_;
};

// What the open window shares: the theme, the app's preset, and when to save
struct State {
    State(Context &c, std::string &p) : context(c), preset(p) {}
    Context &context;
    std::string &preset;
    double changed = -1.0; // when the last unsaved change was made
    std::string message;

    Theme &theme() { return context.theme(); }
    void touch(double now) { changed = now; }
};

}

ui::Theme studioTheme(bool dark, const std::string &preset)
{
    Theme theme = Theme::preset(preset);
    theme.setDepth(3.0f).setSoftness(1.8f).setCornerRadius(10.0f).setCurvature(0.16f).setIntensity(0.85f);
    theme.setRim(0.55f).setControlScale(0.9f).setTextContrast(0.52f);
    theme.setDarkMode(dark);
    return theme;
}

std::vector<std::string> appearancePresets()
{
    return {"Carbon", "Obsidian", "Slate", "Ember", "Midnight", "Graphite", "Dark",
            "Light",  "Ocean",    "Sand",  "Mint",  "Rose",     "Lavender"};
}

std::string themePath() { return (fs::u8path(Settings::filePath()).parent_path() / "theme.toml").string(); }

std::string writeTheme(const Theme &theme, const std::string &preset)
{
    std::ostringstream out;
    out << "# Vibin's look, from View > Appearance\n";
    out << "preset = " << toml::quote(preset) << "\n";
    out << "dark = " << (theme.isDark() ? "true" : "false") << "\n";
    out << "base = " << toml::quote(hexText(theme.baseColor())) << "\n";
    out << "accent = " << toml::quote(hexText(theme.accentColor())) << "\n";
    out << "depth = " << toml::number(theme.depth()) << "\n";
    out << "softness = " << toml::number(theme.softness()) << "\n";
    out << "corner_radius = " << toml::number(theme.cornerRadius()) << "\n";
    out << "shadow_strength = " << toml::number(theme.intensity()) << "\n";
    out << "light_angle = " << toml::number(theme.lightAngle()) << "\n";
    out << "curvature = " << toml::number(theme.curvature()) << "\n";
    out << "edge_highlight = " << toml::number(theme.rim()) << "\n";
    out << "text_contrast = " << toml::number(theme.textContrast()) << "\n";
    out << "text_size = " << toml::number(theme.fontScale()) << "\n";
    out << "control_size = " << toml::number(theme.controlScale()) << "\n";
    out << "spacing = " << toml::number(theme.spacingScale()) << "\n";
    return out.str();
}

bool readTheme(const std::string &text, Theme &theme, std::string &preset, std::string &error)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return false;
    error.clear();
    preset = root.get("preset", "Carbon");
    Theme t = studioTheme(root.get("dark", true), preset);
    Color c;
    if (parseHex(root.get("base", ""), c))
        t.setBaseColor(c);
    else if (root.at("base"))
        error = "base should be a colour like \"#1F2227\"";
    if (parseHex(root.get("accent", ""), c))
        t.setAccentColor(c);
    else if (root.at("accent"))
        error = "accent should be a colour like \"#7C9CFF\"";
    t.setDarkMode(root.get("dark", t.isDark()));
    t.setDepth(static_cast<float>(root.get("depth", static_cast<double>(t.depth()))));
    t.setSoftness(static_cast<float>(root.get("softness", static_cast<double>(t.softness()))));
    t.setCornerRadius(static_cast<float>(root.get("corner_radius", static_cast<double>(t.cornerRadius()))));
    t.setIntensity(static_cast<float>(root.get("shadow_strength", static_cast<double>(t.intensity()))));
    t.setLightAngle(static_cast<float>(root.get("light_angle", static_cast<double>(t.lightAngle()))));
    t.setCurvature(static_cast<float>(root.get("curvature", static_cast<double>(t.curvature()))));
    t.setRim(static_cast<float>(root.get("edge_highlight", static_cast<double>(t.rim()))));
    t.setTextContrast(static_cast<float>(root.get("text_contrast", static_cast<double>(t.textContrast()))));
    t.setFontScale(static_cast<float>(root.get("text_size", static_cast<double>(t.fontScale()))));
    t.setControlScale(static_cast<float>(root.get("control_size", static_cast<double>(t.controlScale()))));
    t.setSpacingScale(static_cast<float>(root.get("spacing", static_cast<double>(t.spacingScale()))));
    theme = t;
    return error.empty();
}

bool loadTheme(Theme &theme, std::string &preset, std::string &error)
{
    std::ifstream in(fs::u8path(themePath()), std::ios::binary);
    if (!in)
        return false;
    std::ostringstream text;
    text << in.rdbuf();
    Theme loaded = theme;
    if (!readTheme(text.str(), loaded, preset, error)) {
        error = "theme.toml, " + error;
        return false;
    }
    theme = loaded;
    return true;
}

bool saveTheme(const Theme &theme, const std::string &preset, std::string &error)
{
    const fs::path path = fs::u8path(themePath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << writeTheme(theme, preset);
    if (!out) {
        error = "Couldn't write " + path.string();
        return false;
    }
    return true;
}

FloatingWindow &openAppearance(Context &context, std::string &preset, std::function<void()> onClose)
{
    const Rect bounds = context.bounds();
    const float width = 440.0f;
    const float height = std::min(780.0f, bounds.h - 110.0f);
    FloatingWindow &window =
        FloatingWindow::open(context, "Appearance", {bounds.x + bounds.w - width - 28.0f, bounds.y + 70.0f, width, height},
                             std::move(onClose));
    Container &body = window.content();
    body.setAlign(Align::Stretch);
    auto state = std::make_shared<State>(context, preset);

    // Saves once changes have stopped for a moment; every control's value follows the theme
    body.add<FrameHook>([state](const Input &input) {
        if (state->changed >= 0.0 && input.time - state->changed > 0.5) {
            state->changed = -1.0;
            std::string error;
            state->message = saveTheme(state->theme(), state->preset, error) ? "Saved" : error;
        }
    });

    auto section = [&body](const char *title) {
        body.add<Label>(title, TextStyle::Body, TextTone::Secondary).setMinSize({0.0f, 30.0f});
    };
    // A labelled slider: get gives 0-1 from the theme, set takes 0-1 into it, text shows the value
    auto slider = [&body, state](const char *label, std::function<float(const Theme &)> get,
                                 std::function<void(Theme &, float)> set, std::function<std::string(const Theme &)> text) {
        auto &row = body.add<Container>(Direction::Row);
        row.setAlign(Align::Center);
        row.add<Label>(label, TextStyle::Caption, TextTone::Secondary).setMinSize({112.0f, 0.0f});
        auto &s = row.add<Slider>(get(state->theme()));
        s.setSize(Size::Small).setGrow(1.0f);
        s.onChange([state, set](float v) {
            set(state->theme(), v);
            state->touch(state->context.window().time());
        });
        row.add<LiveLabel>([state, text] { return text(state->theme()); }, TextStyle::Caption, TextTone::Muted)
            .setMinSize({44.0f, 0.0f});
        Slider *raw = &s;
        row.add<FrameHook>([raw, state, get](const Input &) { raw->setValue(get(state->theme())); });
    };
    auto range = [](float low, float high) {
        return std::make_pair([low, high](float v) { return low + v * (high - low); },
                              [low, high](float x) { return (x - low) / (high - low); });
    };

    // Presets
    section("Presets");
    auto &grid = body.add<Grid>(4);
    for (const std::string &name : appearancePresets()) {
        grid.add<Swatch>(name, [state, name] { return state->preset == name; }, [state, name] {
            const Theme p = Theme::preset(name);
            const bool dark = p.isDark();
            Theme &t = state->theme();
            t.setBaseColor(p.baseColor()).setAccentColor(p.accentColor());
            t.setDarkMode(dark);
            state->preset = name;
            state->touch(state->context.window().time());
        });
    }
    auto &mode = body.add<Container>(Direction::Row);
    mode.setAlign(Align::Center);
    mode.add<Label>("Mode", TextStyle::Caption, TextTone::Secondary).setMinSize({112.0f, 0.0f});
    auto &modes = mode.add<SegmentedControl>(std::vector<std::string>{"Dark", "Light"}, state->theme().isDark() ? 0 : 1,
                                              [state](int i) {
                                                  state->theme().setDarkMode(i == 0);
                                                  state->touch(state->context.window().time());
                                              });
    modes.setSize(Size::Small);
    SegmentedControl *modesRaw = &modes;
    mode.add<FrameHook>([modesRaw, state](const Input &) { modesRaw->setSelected(state->theme().isDark() ? 0 : 1); });

    // Accent
    section("Accent");
    auto &dots = body.add<Container>(Direction::Row);
    dots.setAlign(Align::Center);
    for (uint32_t hex : {0x7C9CFFu, 0xA48CFFu, 0x4FC3B8u, 0x5CC98Au, 0xF0B04Au, 0xF08A4Bu, 0xE0607Eu, 0xE07AC4u}) {
        Theme probe = Theme::light();
        probe.setAccentColor(Color::hex(hex));
        const uint32_t kept = probe.accentColor().toHex(); // as the theme keeps it, to compare
        dots.add<ColorDot>(Color::hex(hex), [state, kept] { return state->theme().accentColor().toHex() == kept; },
                           [state, hex] {
                               state->theme().setAccentColor(Color::hex(hex));
                               state->touch(state->context.window().time());
                           });
    }
    slider(
        "Hue",
        [](const Theme &t) {
            float h, s, l;
            t.accentColor().toHsl(h, s, l);
            return h / 360.0f;
        },
        [](Theme &t, float v) {
            float h, s, l;
            t.accentColor().toHsl(h, s, l);
            t.setAccentColor(Color::hsl(v * 360.0f, std::max(s, 0.6f), l));
        },
        [](const Theme &t) { return hexText(t.accentColor()); });

    // Background
    section("Background");
    slider(
        "Tint",
        [](const Theme &t) {
            float h, s, l;
            t.baseColor().toHsl(h, s, l);
            return h / 360.0f;
        },
        [](Theme &t, float v) {
            float h, s, l;
            t.baseColor().toHsl(h, s, l);
            t.setBaseColor(Color::hsl(v * 360.0f, std::max(s, 0.04f), l));
        },
        [](const Theme &t) {
            float h, s, l;
            t.baseColor().toHsl(h, s, l);
            return format("%.0f", h);
        });
    slider(
        "Colourfulness",
        [](const Theme &t) {
            float h, s, l;
            t.baseColor().toHsl(h, s, l);
            return s / 0.25f;
        },
        [](Theme &t, float v) {
            float h, s, l;
            t.baseColor().toHsl(h, s, l);
            t.setBaseColor(Color::hsl(h, v * 0.25f, l));
        },
        [](const Theme &t) {
            float h, s, l;
            t.baseColor().toHsl(h, s, l);
            return format("%.0f%%", s / 0.25f * 100.0f);
        });
    slider(
        "Brightness",
        [](const Theme &t) {
            float h, s, l, low, high;
            t.baseColor().toHsl(h, s, l);
            band(t.isDark(), low, high);
            return (l - low) / (high - low);
        },
        [](Theme &t, float v) {
            float h, s, l, low, high;
            t.baseColor().toHsl(h, s, l);
            band(t.isDark(), low, high);
            t.setBaseColor(Color::hsl(h, s, low + v * (high - low)));
        },
        [](const Theme &t) { return hexText(t.baseColor()); });

    // Surfaces
    section("Surfaces");
    {
        auto [to, from] = range(2.0f, 10.0f);
        slider("Depth", [from](const Theme &t) { return from(t.depth()); }, [to](Theme &t, float v) { t.setDepth(to(v)); },
               [](const Theme &t) { return format("%.1f", t.depth()); });
    }
    {
        auto [to, from] = range(1.0f, 3.0f);
        slider("Softness", [from](const Theme &t) { return from(t.softness()); },
               [to](Theme &t, float v) { t.setSoftness(to(v)); }, [](const Theme &t) { return format("%.1f", t.softness()); });
    }
    {
        auto [to, from] = range(0.4f, 1.0f);
        slider("Shadow strength", [from](const Theme &t) { return from(t.intensity()); },
               [to](Theme &t, float v) { t.setIntensity(to(v)); },
               [](const Theme &t) { return format("%.0f%%", t.intensity() * 100.0f); });
    }
    {
        auto [to, from] = range(4.0f, 28.0f);
        slider("Corners", [from](const Theme &t) { return from(t.cornerRadius()); },
               [to](Theme &t, float v) { t.setCornerRadius(to(v)); },
               [](const Theme &t) { return format("%.0f px", t.cornerRadius()); });
    }
    {
        auto [to, from] = range(0.0f, 0.6f);
        slider("Curvature", [from](const Theme &t) { return from(t.curvature()); },
               [to](Theme &t, float v) { t.setCurvature(to(v)); },
               [](const Theme &t) { return format("%.0f%%", t.curvature() / 0.6f * 100.0f); });
    }
    slider("Edge highlight", [](const Theme &t) { return t.rim(); }, [](Theme &t, float v) { t.setRim(v); },
           [](const Theme &t) { return format("%.0f%%", t.rim() * 100.0f); });
    slider("Light from", [](const Theme &t) { return t.lightAngle() / 360.0f; },
           [](Theme &t, float v) { t.setLightAngle(std::min(v, 0.999f) * 360.0f); },
           [](const Theme &t) { return format("%.0f deg", t.lightAngle()); });

    // Text and layout
    section("Text and layout");
    {
        auto [to, from] = range(0.35f, 0.6f);
        slider("Contrast", [from](const Theme &t) { return from(t.textContrast()); },
               [to](Theme &t, float v) { t.setTextContrast(to(v)); },
               [from](const Theme &t) { return format("%.0f%%", from(t.textContrast()) * 100.0f); });
    }
    {
        auto [to, from] = range(0.85f, 1.25f);
        slider("Text size", [from](const Theme &t) { return from(t.fontScale()); },
               [to](Theme &t, float v) { t.setFontScale(to(v)); },
               [](const Theme &t) { return format("%.0f%%", t.fontScale() * 100.0f); });
    }
    {
        auto [to, from] = range(0.8f, 1.2f);
        slider("Control size", [from](const Theme &t) { return from(t.controlScale()); },
               [to](Theme &t, float v) { t.setControlScale(to(v)); },
               [](const Theme &t) { return format("%.0f%%", t.controlScale() * 100.0f); });
    }
    {
        auto [to, from] = range(1.0f, 2.0f);
        slider("Spacing", [from](const Theme &t) { return from(t.spacingScale()); },
               [to](Theme &t, float v) { t.setSpacingScale(to(v)); },
               [](const Theme &t) { return format("%.0f%%", t.spacingScale() * 100.0f); });
    }

    auto &footer = body.add<Container>(Direction::Row);
    footer.setAlign(Align::Center);
    footer.add<LiveLabel>([state] { return state->message; }, TextStyle::Caption, TextTone::Muted).setGrow(1.0f);
    footer.add<Button>([state] {
        // The preset's colours on the studio shape
        Theme &t = state->theme();
        t = studioTheme(t.isDark(), state->preset);
        state->touch(state->context.window().time());
    }).setLabel("Reset shape").setVariant(ButtonVariant::Flat).setSize(Size::Small);
    FloatingWindow *raw = &window;
    footer.add<Button>([raw] { raw->close(); }).setLabel("Done").setVariant(ButtonVariant::Accent).setSize(Size::Small);
    return window;
}

}
