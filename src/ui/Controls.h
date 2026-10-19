#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Component.h"

namespace ui {

// Form and display controls. Like the core components, they only pick surfaces and text roles;
// every colour and size comes from the Theme.

// A box that is pressed in when checked, with an accent fill and an optional label beside it
class Checkbox : public Component {
public:
    explicit Checkbox(std::string label = {}, bool checked = false, std::function<void(bool)> onChange = {});

    Checkbox &onChange(std::function<void(bool)> callback);
    Checkbox &setChecked(bool checked);
    Checkbox &setSize(Size size);
    bool isChecked() const { return checked_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float boxSize(const Theme &theme) const;

    std::string label_;
    std::function<void(bool)> onChange_;
    bool checked_;
    float fill_; // animated, 0 to 1
    Size size_ = Size::Medium;
    ClickState click_;
};

// Mutually exclusive options, each an inset circle with an accent dot when selected
class RadioGroup : public Component {
public:
    RadioGroup(std::vector<std::string> options, int selected = 0, std::function<void(int)> onChange = {});

    RadioGroup &onChange(std::function<void(int)> callback);
    RadioGroup &setSelected(int index);
    RadioGroup &setDirection(Direction direction);
    RadioGroup &setSize(Size size);
    int selected() const { return selected_; }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    Vec2 optionSize(const Theme &theme, size_t index) const;
    float gap(const Theme &theme) const;

    std::vector<std::string> options_;
    std::vector<Rect> optionRects_; // laid out in arrange, for hit testing and drawing
    std::function<void(int)> onChange_;
    int selected_;
    Direction direction_ = Direction::Column;
    Size size_ = Size::Medium;
    int hovered_ = -1;
    int pressed_ = -1;
};

// A row of options in an inset track; the selected one is a raised segment that slides across
class SegmentedControl : public Component {
public:
    SegmentedControl(std::vector<std::string> options, int selected = 0, std::function<void(int)> onChange = {});

    SegmentedControl &onChange(std::function<void(int)> callback);
    SegmentedControl &setSelected(int index);
    SegmentedControl &setSize(Size size);
    int selected() const { return selected_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    Rect segment(float position) const;

    std::vector<std::string> options_;
    std::function<void(int)> onChange_;
    int selected_;
    float position_; // animated index of the raised segment
    Size size_ = Size::Medium;
    int hovered_ = -1;
    int pressed_ = -1;
};

// A number between minus and plus buttons, in an inset pill
class Stepper : public Component {
public:
    Stepper(float value = 0.0f, float min = 0.0f, float max = 10.0f, float step = 1.0f,
            std::function<void(float)> onChange = {});

    Stepper &onChange(std::function<void(float)> callback);
    Stepper &setValue(float value);
    Stepper &setSuffix(std::string suffix); // shown after the number, e.g. " kg"
    Stepper &setSize(Size size);
    float value() const { return value_; }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::string format(float value) const;
    void change(float delta);

    float value_, min_, max_, step_;
    std::string suffix_;
    std::function<void(float)> onChange_;
    Size size_ = Size::Medium;
    Rect minus_, plus_;
    ClickState minusClick_, plusClick_;
};

// A raised dial turned by dragging up and down. A ring of dots shows the value; an optional
// formatter puts text in the middle.
class Knob : public Component {
public:
    explicit Knob(float value = 0.5f, std::function<void(float)> onChange = {});

    Knob &onChange(std::function<void(float)> callback);
    Knob &setValue(float value); // clamped to [0, 1]
    Knob &setDiameter(float pixels);
    Knob &setFormatter(std::function<std::string(float)> format);
    float value() const { return value_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float value_;
    float diameter_ = 140.0f;
    std::function<void(float)> onChange_;
    std::function<std::string(float)> format_;
    bool hovered_ = false;
    bool dragging_ = false;
    float dragStartY_ = 0.0f;
    float dragStartValue_ = 0.0f;
};

// An inset track with an accent fill; not interactive
class ProgressBar : public Component {
public:
    explicit ProgressBar(float value = 0.0f);

    ProgressBar &setValue(float value); // clamped to [0, 1]
    ProgressBar &setSize(Size size);
    float value() const { return value_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float value_;
    float shown_; // animated toward value_
    Size size_ = Size::Medium;
};

// A single line of text input. Clicking it takes the window's keyboard focus (pressing outside
// or Escape lets go); clicking places the caret, dragging or Shift-clicking selects, and a
// double-click selects a word. It accepts printable ASCII typing and these keys:
// - Left/Right, Home/End move; with Shift they select; Ctrl+Left/Right move by word.
// - Backspace/Delete remove the selection or a character (a word with Ctrl).
// - Enter submits, Escape lets go of the focus.
// - The edit.selectAll, edit.copy, edit.cut and edit.paste actions (Ctrl+A/C/X/V unless rebound)
//   work while it has the focus. Other shortcuts (Ctrl+S, F1) go on to the app; plain keys
//   such as Space don't, so typing never triggers them.
// Password mode shows asterisks and won't copy or cut.
class TextField : public Component {
public:
    explicit TextField(std::string placeholder = {});

    TextField &setText(std::string text);
    TextField &setPlaceholder(std::string placeholder);
    TextField &setPassword(bool password); // shows asterisks instead of the text
    TextField &setSize(Size size);
    TextField &onChange(std::function<void(const std::string &)> callback);
    TextField &onSubmit(std::function<void(const std::string &)> callback);
    const std::string &text() const { return text_; }
    bool focused() const;
    // Takes the keyboard focus (once it has had input, so it knows its window)
    void focus();
    // The selected range [from, to), equal when nothing is selected
    size_t selectionStart() const { return std::min(anchor_, caret_); }
    size_t selectionEnd() const { return std::max(anchor_, caret_); }
    void selectAll();

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::string shown() const; // the text as displayed
    Rect textBox() const;
    size_t indexAt(float x) const;
    size_t wordStart(size_t from) const;
    size_t wordEnd(size_t from) const;
    void moveCaret(size_t to, bool select);
    bool eraseSelection();
    void insert(const std::string &text);
    void changed();
    bool onKey(const KeyEvent &e);
    void makeHooks();

    std::string text_;
    std::string placeholder_;
    std::function<void(const std::string &)> onChange_;
    std::function<void(const std::string &)> onSubmit_;
    size_t caret_ = 0;
    size_t anchor_ = 0; // the other end of the selection
    size_t scroll_ = 0; // characters scrolled off the left
    bool password_ = false;
    bool hovered_ = false;
    bool dragging_ = false;
    double focusTime_ = 0.0;
    double time_ = 0.0;
    double lastPress_ = -1.0;
    Size size_ = Size::Medium;
    const Theme *theme_ = nullptr;
    Hooks hooks_;
};

// A small pill that can be toggled on, for filters and tags
class Chip : public Component {
public:
    explicit Chip(std::string label, bool selected = false, std::function<void(bool)> onChange = {});

    Chip &onChange(std::function<void(bool)> callback);
    Chip &setSelected(bool selected);
    Chip &setSelectable(bool selectable); // false makes a static tag
    bool isSelected() const { return selected_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::string label_;
    std::function<void(bool)> onChange_;
    bool selected_;
    bool selectable_ = true;
    ClickState click_;
};

// A small accent pill with a count or status, the one place text sits on an accent fill
class Badge : public Component {
public:
    explicit Badge(std::string text);

    Badge &setText(std::string text);

    Vec2 measure(const Theme &theme) const override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::string text_;
};

// An audio level meter: an inset track that fills with the accent colour on a decibel scale,
// with a peak-hold marker that waits a moment and then falls. Feed it the latest peak amplitude
// (linear, 1.0 is full scale) every frame. At or above full scale the whole track lights up.
class LevelMeter : public Component {
public:
    explicit LevelMeter(Direction direction = Direction::Row);

    LevelMeter &setLevel(float amplitude);
    LevelMeter &setRange(float minDb, float maxDb); // default -60 to +6
    LevelMeter &setThickness(float pixels);
    LevelMeter &setLength(float pixels); // preferred length along the track, default 160
    float level() const { return level_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float position(float amplitude) const; // 0-1 along the track

    Direction direction_;
    float minDb_ = -60.0f;
    float maxDb_ = 6.0f;
    float thickness_ = 10.0f;
    float length_ = 160.0f;
    float level_ = 0.0f;   // latest amplitude
    float shown_ = 0.0f;   // 0-1, rises at once and falls smoothly
    float hold_ = 0.0f;    // 0-1, peak hold
    float holdAge_ = 0.0f; // seconds since the hold was set
    float clipAge_ = 1e9f; // seconds since the level last reached full scale
};

// A convex raised circle with initials
class Avatar : public Component {
public:
    explicit Avatar(std::string initials, Size size = Size::Medium);

    Vec2 measure(const Theme &theme) const override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::string initials_;
    Size size_;
};

}
