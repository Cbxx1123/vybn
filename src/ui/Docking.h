#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Components.h"
#include "Context.h"
#include "Layout.h"
#include "UISystem.h"

namespace ui {

class DockSpace;
class DockHost;

// Where a panel goes relative to a docked one: beside it (splitting its place in two) or in the
// middle (as another tab in the same place)
enum class DockSide { Left, Right, Top, Bottom, Center };

// A panel that lives in a DockSpace: docked into its grid, or floating in a window of its own.
// It is a column for its content; header() is a row of controls shown in the panel's header
// (beside its tab when docked, in the window's title bar when floating) while the panel is
// showing. Make panels with DockSpace::addPanel.
class DockPanel : public Container {
public:
    explicit DockPanel(std::string title);

    const std::string &title() const { return title_; }
    Container &header() { return *header_; }
    const Container &header() const { return *header_; }
    // A panel that can be closed (a plugin's interface): a close button on its tab, and its
    // window's close button closes it rather than docking it back. `onClose` runs as it goes,
    // after which the space destroys it.
    void setClosable(std::function<void()> onClose)
    {
        onClose_ = std::move(onClose);
        closable_ = true;
    }
    bool closable() const { return closable_; }

private:
    friend class DockSpace;

    std::string title_;
    bool closable_ = false;
    std::function<void()> onClose_;
    std::unique_ptr<Container> header_;
    // Where it came from when it was floated, for docking it back: a panel that was beside it,
    // which side of that panel it was on, and the share it had
    DockPanel *homeAnchor_ = nullptr;
    DockSide homeSide_ = DockSide::Center;
    float homeShare_ = 0.5f;
};

// A grid of docked panels: places split in two (side by side or stacked, with a handle between
// them that can be dragged) down to cards that each show one panel, with a tab for every panel
// in that place.
//
// Drag a tab out of its card (or press the pop-out button) and the panel lifts out into a
// window of its own on the desktop (a native window, through the UISystem), growing out of the
// card. While a window is dragged by its title bar and the mouse is over the grid, it turns
// see-through and a dial shows on the card under the mouse: its middle docks the panel as
// another tab, its four sides give the panel that half of the card; when the grid is split, a
// knob at the middle of each of its edges gives the panel that side of the whole grid. A
// highlight shows where it will go. Let go over one and the window flies into place and fades
// as the grid makes room; anywhere else it stays where it is. Ctrl hides the targets. The
// window's Dock button, or its close button, puts the panel back where it came from.
//
// A card's tab strip takes drops too: a floating window held over it shows a marker between the
// tabs and goes in as a tab before or after the one it is nearest. Dragging a tab along its strip
// moves it there; only dragging it out of the strip lifts it out into a window.
//
// The space owns its panels wherever they are, and must live as long as `context`, the main
// window it is in. Place and move panels outside input and drawing (it posts its own moves).
class DockSpace : public Component {
public:
    DockSpace(UISystem &system, Context &context);

    // A new panel, not yet placed: dock() it
    DockPanel &addPanel(std::string title);
    // Docks `panel` (which must not be docked already) beside `target` on `side`, or as a tab
    // with it for Center. With no target (or one that isn't docked) it goes on that side of the
    // whole grid. `share` is the part of the split it gets. A floating panel's window closes.
    void dock(DockPanel &panel, DockPanel *target, DockSide side, float share = 0.5f);
    // Floats a docked panel in a window of its own, `frame` in desktop (screen) coordinates
    void float_(DockPanel &panel, Rect frame);
    // Docks a floating panel back where it came from, its window flying there first
    void dockHome(DockPanel &panel);

    // Shows a panel: its tab becomes the current one, or its window comes to the front
    void show(DockPanel &panel);
    // Whether the panel is on screen: its window is open, or it is its card's current tab
    bool isShowing(const DockPanel &panel) const;
    bool isFloating(const DockPanel &panel) const;
    // Gives a docked panel's place `share` of the split it is in (nothing when it fills the grid)
    void setShare(const DockPanel &panel, float share);
    // Closes a closable panel after this frame (its window closes if it floats) and destroys it
    void removePanel(DockPanel &panel);
    // A floating panel's window takes this size (desktop pixels), at once
    void resizeFloating(const DockPanel &panel, float width, float height);
    // Where a floating panel's window is on the desktop (false when it isn't floating)
    bool floatingFrame(const DockPanel &panel, Rect &frame) const;

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    friend class DockHost;

    struct Node {
        Node *parent = nullptr;
        // A split: two children along `direction`, the first taking `ratio` of the room
        std::unique_ptr<Node> first, second;
        Direction direction = Direction::Row;
        float ratio = 0.5f;
        float shownRatio = 0.5f; // glides to `ratio`, so splits open and move smoothly
        // A card: its panels as tabs
        std::vector<DockPanel *> panels;
        size_t current = 0;
        // From the last arrange
        Rect rect, header, content, handle, popOut;
        std::vector<Rect> tabs;
        std::vector<Rect> closes; // closable panels' close buttons, on their tabs (empty rects otherwise)
        float available = 0.0f;
        int hoveredTab = -1;
        ClickState popOutClick;

        bool isSplit() const { return first != nullptr; }
    };
    struct Target {
        Node *node = nullptr; // the card, or the root for the grid's edges
        DockSide side = DockSide::Center;
        Rect preview;
        // A card's tab strip: the panel goes in as a tab before or after `beside`, and `marker` shows where
        bool onTab = false;
        DockPanel *beside = nullptr;
        bool after = false;
        Rect marker;
    };
    struct Floating {
        DockPanel *panel;
        engine::WindowId id;
        DockHost *host;
    };

    Node *leafOf(const DockPanel &panel) const;
    Node *firstLeaf(Node &node) const;
    void insert(DockPanel &panel, Node &node, DockSide side, float share);
    void remove(DockPanel &panel);
    void collapse(Node &leaf);
    void forgetPointers();
    const Floating *floating(const DockPanel &panel) const;

    // Drop targets, in the main window's pixels
    Target targetAt(Vec2 p) const;
    Node *leafAt(Node &node, Vec2 p) const;
    Rect previewFor(const Node &node, DockSide side, float share) const;
    std::array<Vec2, 4> edgeKnobs() const; // left, right, top, bottom of the grid
    float dialRadius() const;

    void arrangeNode(Node &node, const Rect &rect, const Theme &theme);
    void inputNode(Node &node, const Input &input);
    void easeNode(Node &node, float amount);
    void drawNode(const Node &node, DrawList &list, const Theme &theme) const;
    void drawTargets(DrawList &list, const Theme &theme) const;

    // From a window's host while it is dragged; `screen` is the mouse on the desktop. Returns
    // whether the mouse is over the grid.
    bool dragWindow(Vec2 screen, bool allowed);
    void dropWindow(DockHost &host, Vec2 screen, bool allowed);
    // Takes a floating panel out of its window (which closes at the next tick)
    void releaseFloating(DockPanel &panel);
    // Puts a panel in its neighbour's card as a tab, before or after it
    void dockTab(DockPanel &panel, DockPanel &beside, bool after);
    void finishTab(DockPanel &panel, DockPanel &beside, bool after);
    // A tab dragged along its strip swaps places with its neighbours
    void reorderTab(const Input &input);
    // Where a tab is drawn: the pressed one follows the mouse along its strip
    Rect tabRect(const Node &node, size_t i) const;
    // Lifts a panel out of the grid into a native window that grows from `from` to `to`
    // (desktop coordinates), following the mouse if `dragging`
    void floatPanel(DockPanel &panel, Rect from, Rect to, bool dragging, Vec2 grab);
    // The window has landed: close it and dock its panel
    void finishDock(DockPanel &panel, DockPanel *anchor, DockSide side, float share);
    void windowGone(DockPanel &panel);
    Rect toScreen(const Rect &r) const;

    UISystem &system_;
    Context &context_;
    std::vector<std::unique_ptr<DockPanel>> panels_;
    std::unique_ptr<Node> root_;
    std::vector<Floating> floating_;

    // A window being dragged over the grid
    Target target_;
    bool showTargets_ = false;
    Vec2 mouse_;                     // the dragged window's mouse, in this window's pixels
    const Node *dialNode_ = nullptr; // the card showing the dial
    Vec2 dialCenter_;
    float dialShown_ = 0.0f;         // 0-1, the dial growing in and fading out
    std::array<float, 5> hot_{};     // each dial part's highlight, by DockSide
    std::array<float, 4> edgeHot_{};
    float edgesShown_ = 0.0f;
    Rect previewShown_;
    float previewAlpha_ = 0.0f;

    // A tab pressed and maybe being dragged along its strip or out of its card
    DockPanel *pressedTab_ = nullptr;
    Vec2 pressedAt_;
    Rect pressedCard_;
    Node *pressedNode_ = nullptr; // the card the pressed tab is in
    float tabGrab_ = 0.0f;        // the mouse's offset into the pressed tab
    float tabX_ = 0.0f;           // where the pressed tab is drawn, along its strip
    // A split's handle being dragged
    Node *dragging_ = nullptr;
    Node *hoveredHandle_ = nullptr;
    float dragOffset_ = 0.0f;
};

// The content of a floating panel's native window: the panel itself, moved and sized with the
// window. It drags the window by its title bar, reading the mouse on the desktop each frame, and
// animates it: growing out of the card it came from, turning see-through over the grid, and
// flying into place and fading when it docks.
class DockHost : public Component {
public:
    DockHost(DockSpace &space, DockPanel &panel, engine::Window &window, TitleBar &titleBar, Rect from, Rect to,
             bool dragging, Vec2 grab);

    // Null once the panel has gone back to the grid
    DockPanel *panel() const { return panel_; }
    bool busy() const { return dragging_ || landing_; }
    bool landing() const { return landing_; }
    // Flies the window to `target` (desktop coordinates) while fading it out, then runs `then`
    void land(Rect target, std::function<void()> then);
    void detach() { panel_ = nullptr; }
    void focus() { window_.focus(); }
    void resizeTo(float width, float height);
    Rect shown() const { return shown_; }

    Vec2 measure(const Theme &) const override { return {}; }
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    void apply();

    DockSpace &space_;
    DockPanel *panel_;
    engine::Window &window_;
    TitleBar &titleBar_;
    Rect shown_;  // where the window is, glided toward target_
    Rect target_;
    float opacity_ = 0.0f;
    float opacityTarget_ = 1.0f;
    bool morphing_ = true; // still gliding into its first place
    bool dragging_;
    Vec2 grab_;            // the mouse's offset from the window's corner while dragging
    bool landing_ = false;
    float landingTime_ = 0.0f;
    std::function<void()> landed_;
    int frames_ = 0;
    int appliedX_ = 0, appliedY_ = 0, appliedW_ = 0, appliedH_ = 0;
    float appliedOpacity_ = -1.0f;
};

}
