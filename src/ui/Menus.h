#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Context.h"

namespace ui {

// One entry of a menu: an action, a check item, a submenu or a separator. An item can name a
// Keymap action ("project.save"): the menu shows that action's keys on the right, and a MenuBar
// runs the item when the action is triggered (by its keys, or InputSignals::trigger). An item
// with an action id and no callback triggers the action when picked.
struct MenuItem {
    std::string label;
    std::function<void()> action;
    std::string actionId;
    std::vector<MenuItem> submenu; // non-empty: opens a submenu instead of acting
    std::function<bool()> checked; // set: a check item, marked while this returns true
    std::function<bool()> enabled; // set: greyed out and inert while this returns false
    std::function<std::string()> dynamicLabel; // set: the label, asked for each time the menu opens
    bool separator = false;

    static MenuItem item(std::string label, std::function<void()> action, std::string actionId = {});
    static MenuItem toggle(std::string label, std::function<bool()> checked, std::function<void()> action,
                           std::string actionId = {});
    static MenuItem sub(std::string label, std::vector<MenuItem> items);
    static MenuItem divider();
    // Enabled only while `condition` returns true
    MenuItem when(std::function<bool()> condition) const;
    // Labelled by `labelOf` each time the menu opens ("Undo Delete notes")
    MenuItem labelled(std::function<std::string()> labelOf) const;

    bool isEnabled() const { return !separator && (!enabled || enabled()); }
};

// A popup list of menu items, used as an overlay (see openContextMenu, MenuBar, Dropdown).
// Hovering a submenu item opens it beside this one. Arrow keys move, Enter picks, Right and
// Left open and close submenus, Escape closes. Picking an item closes the whole menu. While it
// is open it takes every key and action (Priority::Popup), so shortcuts wait until it closes.
class MenuPopup : public Component {
public:
    MenuPopup(Context &context, std::vector<MenuItem> items, Vec2 anchor, float minWidth = 0.0f);

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &window, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;
    bool hitTest(Vec2 p) const override;

private:
    float rowHeight(const Theme &theme, const MenuItem &item) const;
    void openSubmenu(int index);
    void activate(int index);
    void move(int step);
    void keyPressed(Key key);
    MenuPopup &root() { return root_ ? *root_ : *this; }

    Context &context_;
    std::vector<MenuItem> items_;
    Vec2 anchor_;
    float minWidth_;
    std::vector<Rect> rows_;
    int highlighted_ = -1;
    float reveal_ = 0.0f; // 0 closed to 1 open: the menu unfolds from its top edge when it opens
    Vec2 lastMouse_{-1.0f, -1.0f};
    std::unique_ptr<MenuPopup> submenu_;
    int submenuIndex_ = -1;
    MenuPopup *root_ = nullptr; // the top-level menu, which is the overlay
    bool closeRequested_ = false; // a submenu asking its parent to close it (Left key)
    Hooks hooks_;                 // the top-level menu's, for the keyboard
};

// Opens `items` as a menu at `at` (framebuffer pixels), e.g. where the mouse was right-clicked
MenuPopup &openContextMenu(Context &context, std::vector<MenuItem> items, Vec2 at);

// A row of menu titles, as in an application's menu bar; it fits in a TitleBar. Clicking a title
// opens its menu, and while one is open, hovering another title switches to it. It hooks the
// action of every item that names one (Priority::App), so their keys work from anywhere in the
// window, and from other windows through their fallback, unless something ahead of it (a focused
// text field, a dialog, an open menu) takes them first. A disabled item's action goes unhandled.
class MenuBar : public Component {
public:
    explicit MenuBar(Context &context);

    MenuBar &addMenu(std::string title, std::vector<MenuItem> items);
    // Opens a menu as if its title were clicked
    void openMenu(int index);

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    struct Menu {
        std::string title;
        std::vector<MenuItem> items;
        Rect rect;
    };
    Context &context_;
    std::vector<Menu> menus_;
    int open_ = -1;
    int hovered_ = -1;
    int generation_ = 0; // tells a closing popup's callback whether it is still the open one
    MenuPopup *popup_ = nullptr;
    std::vector<env::Connection> actions_;
};

// A column that opens a context menu when right-clicked anywhere inside it. The items are built
// at click time, so they can reflect the current state. Inner areas take the click first.
class ContextMenuArea : public Container {
public:
    ContextMenuArea(Context &context, std::function<std::vector<MenuItem>()> items);

    void handleInput(const Input &input) override;

private:
    Context &context_;
    std::function<std::vector<MenuItem>()> items_;
};

// A button showing the selected option; clicking it opens the options as a menu below it
class Dropdown : public Component {
public:
    Dropdown(Context &context, std::vector<std::string> options, int selected = 0, std::function<void(int)> onChange = {});

    Dropdown &onChange(std::function<void(int)> callback);
    Dropdown &setSelected(int index);
    Dropdown &setOptions(std::vector<std::string> options); // keeps the selection's index
    Dropdown &setSize(Size size);
    const std::vector<std::string> &options() const { return options_; }
    int selected() const { return selected_; }

    Vec2 measure(const Theme &theme) const override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    Context &context_;
    std::vector<std::string> options_;
    int selected_;
    std::function<void(int)> onChange_;
    Size size_ = Size::Medium;
    ClickState click_;
    bool open_ = false;
    int generation_ = 0;
};

}
