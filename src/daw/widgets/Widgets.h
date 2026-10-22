#pragma once

// Small controls the DAW's views share. Like every component they only pick surfaces and text
// roles; the Theme decides how they look. Controls that edit the project take a getter and a
// setter instead of holding a value, so every view of the same setting stays in step.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ui/DrawCache.h"
#include "ui/UI.h"

namespace daw {

// The latest peak level of each output channel (linear). The audio system's meters have one
// reader, so the app reads them once per frame into this and every view shows it.
struct StereoLevels {
    float peak[2] = {};
};

// A compact square toggle with a letter or short word: mute, solo, arm, metronome. Raised when
// off; pressed in with accent text when on.
class SmallToggle : public ui::Component {
public:
    SmallToggle(std::string label, std::function<bool()> get, std::function<void(bool)> set);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    std::string label_;
    std::function<bool()> get_;
    std::function<void(bool)> set_;
    bool on_ = false;
    ui::ClickState click_;
    ui::DrawCache cache_{"SmallToggle"}; // draw, while nothing it reads has changed
};

// The small chevron that folds a group: pointing down while the group is open, right while folded.
// Clicking it calls `set` with the opposite of `folded`.
class FoldChevron : public ui::Component {
public:
    FoldChevron(std::function<bool()> folded, std::function<void(bool)> set);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    std::function<bool()> folded_;
    std::function<void(bool)> set_;
    ui::ClickState click_;
    ui::DrawCache cache_{"FoldChevron"}; // draw, while its state is the same
};

class RadialMenu;

// One toggle of a RadialToggles: its letter, its name (shown while hovered in the menu), the key
// that flips it while the menu is open, and its getter and setter
struct RadialToggle {
    std::string label; // "M"
    std::string name;  // "Mute"
    engine::Key key = engine::Key::Unknown;
    std::function<bool()> get;
    std::function<void(bool)> set;
};

// A row of toggles folded into one small raised disc: a dot for each toggle around its rim, lit
// while that toggle is on, at the place its button takes in the menu. Pressing it opens a
// RadialMenu of the toggles around it: drag onto one and let go to flip it (the menu closes), or
// let go on the disc and click them (the menu stays open until a click outside it, on its middle,
// or Escape).
class RadialToggles : public ui::Component {
public:
    explicit RadialToggles(std::vector<RadialToggle> toggles);

    // Where toggle `i` of `count` sits around a centre: an angle in radians, the first at the top
    // and the rest clockwise
    static float angle(size_t i, size_t count);
    bool menuOpen() const { return *open_; }
    // Opens the menu in `context` around the disc (a press does it in the window being used);
    // `held`: the press that opened it is still down
    RadialMenu &openMenu(ui::Context &context, bool held);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;
    bool hitTest(ui::Vec2 p) const override;

private:
    std::vector<RadialToggle> toggles_;
    std::vector<char> on_;
    bool hovered_ = false;
    std::shared_ptr<bool> open_ = std::make_shared<bool>(false); // cleared by the menu as it closes
    ui::DrawCache cache_{"RadialToggles"};
};

// The menu a RadialToggles opens, an overlay around `centre`: a raised disc holding a button per
// toggle on a ring (pressed in with accent text while on) round an inset middle that closes it,
// and the hovered toggle's name in a pill beneath. Its buttons glide out from the middle as it
// opens. `held` says the press that opened it is still down, so letting go over a button after
// dragging flips it and closes the menu. While open, a toggle's key flips it and Escape closes.
class RadialMenu : public ui::Component {
public:
    static constexpr float kButton = 30.0f; // a button's diameter
    static constexpr float kRing = 40.0f;   // from the middle to each button's centre
    static constexpr float kMiddle = 26.0f; // the middle's diameter

    RadialMenu(ui::Context &context, std::vector<RadialToggle> toggles, ui::Vec2 centre, bool held);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &window, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;
    bool hitTest(ui::Vec2 p) const override;

private:
    float radius() const { return kRing + kButton * 0.5f + 8.0f; } // the disc's
    ui::Rect button(size_t i) const;                                // where button i is now
    void flip(size_t i);
    void close();

    ui::Context &context_;
    std::vector<RadialToggle> toggles_;
    ui::Vec2 anchor_; // where it was asked to open
    ui::Vec2 centre_; // where it is, kept inside the window
    bool held_;
    bool dragged_ = false; // the held press has moved off the middle
    int hovered_ = -1;     // the button under the mouse
    bool middleHovered_ = false;
    float reveal_ = 0.0f; // 0 folded into the middle to 1 open
    ui::Hooks hooks_;
};

// A vertical mixing fader: an inset slot with an accent fill up to a raised cap, and dB marks
// beside it. Drag the cap (or press the slot to jump), use the wheel over it, or double-click to
// return to the default. Positions are 0-1; see faderGain for the taper.
class Fader : public ui::Component {
public:
    Fader(std::function<float()> get, std::function<void(float)> set);

    Fader &setMaxGain(float gain);       // gain at the top, for the dB marks
    Fader &setDefault(float position);   // where a double-click puts it

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    float travelTop() const;
    float travelLength() const;
    float capY(float position) const; // centre of the cap
    void change(float position);

    std::function<float()> get_;
    std::function<void(float)> set_;
    float value_ = 0.0f;
    float maxGain_ = 2.0f;
    float default_ = 0.794f; // unity with maxGain 2
    bool hovered_ = false;
    bool dragging_ = false;
    float grab_ = 0.0f; // mouse offset from the cap's centre while dragging
    double lastPress_ = -1.0;
};

// A readout in an inset well: a large line over a small one. The text comes from callbacks
// read every frame; `widest` sizes it so it doesn't change width as the text changes.
class Readout : public ui::Component {
public:
    Readout(std::function<std::string()> main, std::function<std::string()> sub, std::string widest);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    std::function<std::string()> mainText_;
    std::function<std::string()> subText_;
    std::string widest_;
    std::string main_;
    std::string sub_;
};

// A value in an inset pill that is changed by dragging up or down on it (a step every few
// pixels), the wheel, or the arrow keys' callers: `step(steps, fine)` is told how many steps and
// whether Shift was held. The label sits at its left, the value (from a callback each frame) at
// its right; `enabled` greys it out and stops it.
class ScrubField : public ui::Component {
public:
    ScrubField(std::string label, std::function<std::string()> value, std::function<void(int steps, bool fine)> step,
               std::function<bool()> enabled = {});

    // Room for the widest value it will show
    ScrubField &setValueWidth(float width);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    std::string label_;
    std::function<std::string()> value_;
    std::function<void(int, bool)> step_;
    std::function<bool()> enabled_;
    std::string shown_;
    float valueWidth_ = 48.0f;
    bool on_ = true;
    bool hovered_ = false;
    bool dragging_ = false;
    float pressY_ = 0.0f;
    int stepsDone_ = 0;
};

// A small light that is lit while a callback says so (MIDI arriving, recording)
class ActivityLight : public ui::Component {
public:
    explicit ActivityLight(std::function<bool()> lit) : lit_(std::move(lit)) {}

    ui::Vec2 measure(const ui::Theme &) const override { return {10.0f, 10.0f}; }
    void handleInput(const ui::Input &) override { on_ = lit_ && lit_(); }
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    std::function<bool()> lit_;
    bool on_ = false;
};

// A label whose text comes from a callback, read every frame
// The app's mark in the title bar: a raised disc with a waveform in the accent colour. It is a
// Label (with no text) so the title bar treats it as empty, for dragging the window.
class LogoMark : public ui::Label {
public:
    LogoMark() = default;

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;
};

// The project's name in an inset pill, with an accent dot while it has unsaved changes. A Label,
// so it drags the window like the rest of the title bar.
class ProjectBadge : public ui::Label {
public:
    ProjectBadge(std::function<std::string()> name, std::function<bool()> modified);

    void handleInput(const ui::Input &input) override;
    ui::Vec2 measure(const ui::Theme &theme) const override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    std::function<std::string()> name_;
    std::function<bool()> modified_;
    bool dirty_ = false;
};

// Runs a callback every frame with the frame's input; takes no space
class FrameHook : public ui::Component {
public:
    explicit FrameHook(std::function<void(const ui::Input &)> hook) : hook_(std::move(hook)) {}

    ui::Vec2 measure(const ui::Theme &) const override { return {}; }
    void handleInput(const ui::Input &input) override { hook_(input); }

private:
    std::function<void(const ui::Input &)> hook_;
};

class LiveLabel : public ui::Label {
public:
    LiveLabel(std::function<std::string()> text, ui::TextStyle style, ui::TextTone tone = ui::TextTone::Primary);

    void handleInput(const ui::Input &input) override;

private:
    std::function<std::string()> source_;
};

}
