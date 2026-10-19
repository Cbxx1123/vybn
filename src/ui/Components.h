#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Component.h"
#include "DrawCache.h"
#include "Layout.h"

namespace ui {

// Component library. Components choose *which* neumorphic surface to draw (elevation, shape,
// finish, depth, state); the Theme decides how it looks.

// Small vector icons drawn from flat ink shapes, in a text tone
enum class Icon {
    None, Menu, Plus, Minus, Play, Pause, Next, Previous, Stop, Dot, Grid, List, Chevron,
    Close, Maximize, Restore, // window buttons
    ChevronDown, PopOut, Dock, Check,
    Download, Folder, Wave, Keys, // the asset browser: download, a folder, a sound, an instrument
    Gear, Sliders,                // settings, and the appearance controls
    Up, File, Search,             // the file dialog: the parent folder, a file, the search field
};

// Draws `icon` centred in `box` (sized by its shorter side)
void drawIcon(DrawList &list, const Theme &theme, Icon icon, const Rect &box, TextTone tone = TextTone::Primary,
              bool enabled = true);

enum class ButtonVariant {
    Raised, // raised at rest, pressed in when held
    Flat,   // level with the background until hovered
    Accent, // raised, with an accent dot (or accent icon) marking it as the primary action
    Inset,  // pressed into the background; lifts level when held
};

class Button : public Component {
public:
    explicit Button(std::function<void()> onClick = {});

    Button &onClick(std::function<void()> callback);
    Button &setVariant(ButtonVariant variant);
    Button &setShape(Shape shape);
    Button &setSize(Size size);
    Button &setFinish(Finish finish);
    Button &setDepth(Depth depth);
    // Text centred on the button; its font size follows the button's Size. Empty for none.
    Button &setLabel(std::string label);
    // Drawn before the label, or alone in the middle
    Button &setIcon(Icon icon);
    Button &setTone(TextTone tone); // label and icon colour
    // A selected button stays pressed in, for toggle buttons and the current item of a group
    Button &setSelected(bool selected);
    bool isSelected() const { return selected_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::function<void()> onClick_;
    std::string label_;
    Icon icon_ = Icon::None;
    TextTone tone_ = TextTone::Primary;
    ButtonVariant variant_ = ButtonVariant::Raised;
    Shape shape_ = Shape::Rounded;
    Size size_ = Size::Medium;
    Finish finish_ = Finish::Flat;
    Depth depth_ = Depth::Normal;
    bool selected_ = false;
    ClickState click_;
};

class Toggle : public Component {
public:
    explicit Toggle(bool on = false, std::function<void(bool)> onChange = {});

    Toggle &onChange(std::function<void(bool)> callback);
    Toggle &setOn(bool on);
    Toggle &setSize(Size size);
    bool isOn() const { return on_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::function<void(bool)> onChange_;
    bool on_;
    float knob_; // animated knob position, 0 (off) to 1 (on)
    Size size_ = Size::Medium;
    ClickState click_;
};

class Slider : public Component {
public:
    explicit Slider(float value = 0.0f, std::function<void(float)> onChange = {});

    Slider &onChange(std::function<void(float)> callback);
    Slider &setValue(float value); // clamped to [0, 1]
    Slider &setSize(Size size);
    float value() const { return value_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float knobSize() const;
    float trackStart() const;
    float trackLength() const;

    std::function<void(float)> onChange_;
    float value_;
    Size size_ = Size::Medium;
    bool hovered_ = false;
    bool dragging_ = false;
};

// Text drawn straight on the background. One line, or word-wrapped to a width.
class Label : public Component {
public:
    explicit Label(std::string text = {}, Size size = Size::Medium);
    Label(std::string text, TextStyle style, TextTone tone = TextTone::Primary);

    Label &setText(std::string text);
    Label &setSize(Size size);       // control-sized text, matching buttons of that Size
    Label &setStyle(TextStyle style); // typographic scale; overrides setSize
    Label &setTone(TextTone tone);
    Label &setAlignment(TextAlign align); // within the rect, when it's wider than the text
    // Wraps at word boundaries to lines no wider than this (0 keeps one line)
    Label &setWrapWidth(float width);
    // Cuts a single line short with "..." so it fits this width (0 keeps the whole line)
    Label &setMaxWidth(float width);
    const std::string &text() const { return text_; }

    Vec2 measure(const Theme &theme) const override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float fontSize(const Theme &theme) const;
    float maxWidth_ = 0.0f;
    // The text broken into lines, kept until the text, size or wrap width changes
    const std::vector<std::string> &lines(const Theme &theme) const;
    mutable std::vector<std::string> lines_;
    mutable std::string linesText_;
    mutable float linesPx_ = -1.0f;
    mutable float linesWrap_ = -1.0f;
    mutable float linesMax_ = -1.0f;
    mutable float linesWidth_ = 0.0f; // the widest line

    std::string text_;
    Size size_ = Size::Medium;
    std::optional<TextStyle> style_;
    TextTone tone_ = TextTone::Primary;
    TextAlign align_ = TextAlign::Start;
    float wrapWidth_ = 0.0f;
    ui::DrawCache cache_{"Label"}; // draw, while nothing it reads has changed
};

// A card that holds other components. Raised by default; Inset makes a well.
class Panel : public Container {
public:
    explicit Panel(Direction direction = Direction::Column, Elevation elevation = Elevation::Raised);

    Panel &setElevation(Elevation elevation);
    Panel &setFinish(Finish finish);
    Panel &setDepth(Depth depth);
    Panel &setShape(Shape shape); // Rounded or Soft
    // Clip children to the panel (on by default). The padding leaves room for their shadows.
    Panel &setClipContent(bool clip);

    void draw(DrawList &list, const Theme &theme) const override;

protected:
    float padding(const Theme &theme) const override;

private:
    Elevation elevation_;
    Finish finish_ = Finish::Flat;
    Depth depth_ = Depth::Deep;
    Shape shape_ = Shape::Rounded;
    bool clip_ = true;
};

// Empty space that takes a share of leftover room; use it to push siblings apart
class Spacer : public Component {
public:
    explicit Spacer(float grow = 1.0f);

    Vec2 measure(const Theme &) const override { return {}; }
};

// A thin groove separating groups; stretches across its parent by default
class Divider : public Component {
public:
    Divider();

    Vec2 measure(const Theme &theme) const override;
    void draw(DrawList &list, const Theme &theme) const override;
};

}
