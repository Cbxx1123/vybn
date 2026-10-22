#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Components.h"
#include "Layout.h"
#include "engine/Window.h"

namespace ui {

// A full-width row for menus, playlists and settings: optional icon, a title with an optional
// subtitle under it, and optional trailing text. Selected rows are pressed in.
class ListItem : public Component {
public:
    explicit ListItem(std::string title, std::function<void()> onClick = {});

    ListItem &onClick(std::function<void()> callback);
    ListItem &setTitle(std::string title);
    ListItem &setSubtitle(std::string subtitle);
    ListItem &setTrailing(std::string trailing);
    ListItem &setIcon(Icon icon);
    ListItem &setSelected(bool selected);
    ListItem &setSize(Size size);
    bool isSelected() const { return selected_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::string title_;
    std::string subtitle_;
    std::string trailing_;
    Icon icon_ = Icon::None;
    std::function<void()> onClick_;
    bool selected_ = false;
    Size size_ = Size::Medium;
    ClickState click_;
};

// A column for ListItems, packed tighter than the usual gap: rows have no shadow at rest
// (only when hovered), and selected rows are inset, so neighbours don't collide
class ListView : public Container {
public:
    ListView();

protected:
    float gap(const Theme &theme) const override;
};

// Shows one child at a time, filling the whole rect: a page switcher
class Pages : public Container {
public:
    Pages();

    Pages &setCurrent(size_t index);
    size_t current() const { return current_; }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    size_t current_ = 0;
};

// A clipped window onto larger content, scrolled with the mouse wheel (Shift or a sideways
// wheel for horizontal) or by dragging the scrollbars. The content is a column at least as
// large as the view, so Justify and Align still place small content, and never narrower than
// it wants to be: when it doesn't fit across, it scrolls across rather than being squeezed.
// A ScrollView takes the height its parent gives it: give it grow, Align::Stretch or
// setViewHeight. The content is inset by the theme's shadow extent so its shadows stay inside
// the clip.
class ScrollView : public Component {
public:
    ScrollView();

    Container &content() { return *content_; }
    // Swap the content for another column, e.g. to move it into another window
    std::unique_ptr<Container> takeContent();
    void setContent(std::unique_ptr<Container> content);

    // Preferred height of the view (0, the default, asks for none and relies on the parent)
    ScrollView &setViewHeight(float height);
    // Asks for no width, so the parent's width is the view's: content wider than it scrolls across
    // rather than widening the parent (a settings page keeps the dialog's width whatever it holds)
    ScrollView &setFillWidth(bool fill = true);
    // Pixels from the top and left, clamped when next laid out
    ScrollView &scrollTo(float y, float x = 0.0f);
    float offsetY() const { return offset_[1]; }
    float offsetX() const { return offset_[0]; }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    // Axis 0 is horizontal, 1 vertical
    Rect track(int axis) const;
    Rect thumb(int axis) const;

    std::unique_ptr<Container> content_;
    float viewHeight_ = 0.0f;
    bool fillWidth_ = false;
    float offset_[2] = {};    // current, eased toward target_
    float target_[2] = {};
    float maxOffset_[2] = {}; // content size beyond the view
    float contentSize_[2] = {};
    float view_[2] = {};      // visible size of the content area
    float inset_ = 0.0f;
    bool barHovered_[2] = {};
    int dragging_ = -1; // axis whose thumb is being dragged
    float dragStart_ = 0.0f;
    float dragStartOffset_ = 0.0f;
};

// The window's title bar, drawn by the app on the background colour so it blends into the
// page. It's a row: add the app's own controls to it. When the window uses a custom title bar
// (engine::Window::useCustomTitleBar) the minimise, maximise and close buttons follow at the
// right end; otherwise the system frame has them and they're left out. Pass isDragArea to the
// window as its caption test.
class TitleBar : public Container {
public:
    explicit TitleBar(engine::Window &window);

    // Draws the bar as a raised strip `inset` pixels in from each side, its controls inside it
    // (flat, the default, it is part of the page)
    TitleBar &setRaised(bool raised, float inset = 0.0f);

    // Whether a press at `p` (framebuffer pixels) should drag the window: anywhere level with
    // the bar or above it, except on its controls. Labels and spacers count as empty.
    bool isDragArea(Vec2 p) const;

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    bool showButtons() const { return window_.hasCustomTitleBar(); }

    engine::Window &window_;
    std::unique_ptr<Container> buttons_;
    Button *maximize_ = nullptr;
    bool raised_ = false;
    float inset_ = 0.0f;
    Rect bar_; // the raised strip
    static constexpr float kPadX = 12.0f, kPadY = 6.0f;
};

// Main content with a drawer that slides out over it from the left edge. While the drawer is
// open the content is veiled by a scrim and gets no input; clicking outside the drawer or
// pressing Escape closes it. Clicks outside this layout's own rect (a title bar above it, say)
// are left alone.
class DrawerLayout : public Component {
public:
    DrawerLayout();

    Container &content() { return *content_; }
    Panel &drawer() { return *drawer_; }

    DrawerLayout &setDrawerWidth(float width);
    void open() { open_ = true; }
    void close() { open_ = false; }
    void toggle() { open_ = !open_; }
    bool isOpen() const { return open_; }
    // Jumps straight to open or closed, skipping the slide (e.g. at start-up)
    void snap() { progress_ = open_ ? 1.0f : 0.0f; }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    std::unique_ptr<Container> content_;
    std::unique_ptr<Panel> drawer_;
    float width_ = 300.0f;
    float progress_ = 0.0f; // 0 closed, 1 open
    bool open_ = false;
    Hooks hooks_;
};

}
