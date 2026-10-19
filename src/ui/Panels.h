#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Components.h"
#include "Layout.h"
#include "Navigation.h"

namespace ui {

// Two panes side by side (Direction::Row) or stacked (Column), with a handle between them that
// can be dragged to resize them. Split views nest. Each pane is a column whose children
// stretch across it; panes don't scroll, so put a ScrollView in one that may overflow.
// A split view asks for little space of its own: give it grow or Align::Stretch.
class SplitView : public Component {
public:
    explicit SplitView(Direction direction = Direction::Row, float ratio = 0.5f);

    Container &first() { return *first_; }
    Container &second() { return *second_; }

    SplitView &setRatio(float ratio); // share of the space for the first pane, 0-1
    SplitView &setMinimums(float first, float second); // pixels along the split
    float ratio() const { return ratio_; }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    float along(Vec2 v) const { return direction_ == Direction::Row ? v.x : v.y; }

    std::unique_ptr<Container> first_;
    std::unique_ptr<Container> second_;
    Direction direction_;
    float ratio_;
    float minFirst_ = 140.0f;
    float minSecond_ = 140.0f;
    Rect handle_;
    float available_ = 0.0f; // length shared by the panes, from the last arrange
    bool hovered_ = false;
    bool dragging_ = false;
    float dragOffset_ = 0.0f;
};

// Tabs over a raised content card showing the current tab's page. Tabs can be closable, with
// a close button that removes them. Each tab's page is a column; put a ScrollView in it if its
// content may overflow.
class TabView : public Component {
public:
    TabView();

    Container &addTab(std::string title, bool closable = false);
    TabView &setCurrent(size_t index);
    TabView &onTabClosed(std::function<void(const std::string &title)> callback);
    size_t current() const { return current_; }
    size_t tabCount() const { return tabs_.size(); }

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

private:
    struct Tab {
        std::string title;
        bool closable;
        Rect rect;
        Rect close;
    };
    float tabWidth(const Theme &theme, const Tab &tab) const;
    void removeTab(size_t index);

    std::vector<Tab> tabs_;
    std::unique_ptr<Pages> pages_;
    std::function<void(const std::string &)> onTabClosed_;
    size_t current_ = 0;
    Rect strip_;
    Rect body_;
    int hovered_ = -1;
    int pressed_ = -1;
    int closeHovered_ = -1;
    int closePressed_ = -1;
};

}
