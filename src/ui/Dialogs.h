#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Context.h"

namespace ui {

// A modal dialog: a raised card in the middle of the window over a veil that blocks the rest of
// the UI. It has a title, an optional message, room for more content in body(), and a row of
// buttons; every button closes it after running its action. Escape and the close button close
// it too. Open it with Dialog::open.
class Dialog : public Component {
public:
    Dialog(Context &context, std::string title, std::string message = {});

    static Dialog &open(Context &context, std::string title, std::string message = {});

    Container &body() { return *body_; }
    Dialog &addButton(std::string label, std::function<void()> action = {}, bool primary = false);
    Dialog &setWidth(float width);
    // Runs once when it closes, however that happens (a button, Escape, the close button)
    Dialog &onClose(std::function<void()> handler);
    void close();

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &window, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;
    bool hitTest(Vec2 p) const override { return panel_->rect().contains(p); }

private:
    Context &context_;
    std::unique_ptr<Panel> panel_;
    Label *message_ = nullptr;
    Container *body_ = nullptr;
    Container *buttons_ = nullptr;
    float width_ = 440.0f;
    float shown_ = 0.0f; // animated, 0 to 1
    std::function<void()> onClose_;
    env::Connection keys_, actions_;
};

// A window inside the app: a raised card that floats above the page. Drag it by its header,
// resize it from the bottom-right corner, and click anywhere on it to bring it to the front.
// Its content scrolls. With an onPopOut handler it also gets a pop-out button (see
// UISystem::popOut, which moves the content into a native window of its own). Open it with
// FloatingWindow::open.
class FloatingWindow : public Component {
public:
    FloatingWindow(Context &context, std::string title, Rect frame);

    // onClose runs when it closes, however that happens
    static FloatingWindow &open(Context &context, std::string title, Rect frame, std::function<void()> onClose = {});

    Container &content() { return scroll_->content(); }
    std::unique_ptr<Container> takeContent() { return scroll_->takeContent(); }
    void setContent(std::unique_ptr<Container> content) { scroll_->setContent(std::move(content)); }

    FloatingWindow &onPopOut(std::function<void(FloatingWindow &)> handler);
    const std::function<void(FloatingWindow &)> &popOutHandler() const { return popOut_; }
    const std::string &title() const { return title_; }
    Rect frame() const { return frame_; }
    // A new place and size (between frames)
    void setFrame(const Rect &frame) { frame_ = frame; }
    void close();

    Vec2 measure(const Theme &) const override { return {frame_.w, frame_.h}; }
    void arrange(const Rect &window, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    Rect grip() const;

    Context &context_;
    std::string title_;
    Rect frame_;
    Rect header_;
    Rect closeRect_;
    Rect popOutRect_;
    std::unique_ptr<ScrollView> scroll_;
    std::function<void(FloatingWindow &)> popOut_;
    ClickState closeClick_;
    ClickState popOutClick_;
    bool dragging_ = false;
    bool resizing_ = false;
    bool gripHovered_ = false;
    Vec2 grab_; // mouse offset from the frame's corner while dragging or resizing
};

}
