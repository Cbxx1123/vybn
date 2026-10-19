#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "Component.h"

namespace ui {

// Layout layer: stacks children in a row or column.
// Gap and padding never go below what the theme's shadows need (Theme::spacing / Theme::padding
// for panels), so neumorphic shadows always have room. Users can add space, not remove it.
class Container : public Component {
public:
    explicit Container(Direction direction = Direction::Column);

    // Creates a child in place and returns it for configuration
    template <typename T, typename... Args>
    T &add(Args &&...args)
    {
        auto child = std::make_unique<T>(std::forward<Args>(args)...);
        T &ref = *child;
        children_.push_back(std::move(child));
        treeChanged();
        return ref;
    }

    // Takes ownership of an existing component and appends it
    Component &adopt(std::unique_ptr<Component> child)
    {
        children_.push_back(std::move(child));
        treeChanged();
        return *children_.back();
    }
    // Removes `child` and hands it back (nullptr if it isn't a child of this container)
    std::unique_ptr<Component> release(const Component &child);
    // Removes every child.
    // Changing children is only safe outside the tree's input and drawing: use Context::post.
    void clear()
    {
        children_.clear();
        treeChanged();
    }
    size_t childCount() const { return children_.size(); }
    Component &child(size_t index) { return *children_[index]; }

    Container &setDirection(Direction direction);
    Container &setAlign(Align align);       // children on the cross axis
    Container &setJustify(Justify justify); // children on the main axis, when nothing grows
    Container &setGap(float gap);           // raised to Theme::spacing() if smaller
    Container &setPadding(float padding);   // raised to the container's themed minimum if smaller

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;
    void handleInput(const Input &input) override;
    void draw(DrawList &list, const Theme &theme) const override;

protected:
    virtual float gap(const Theme &theme) const;
    // Plain containers only group children, so they need no inset; surfaces override this
    virtual float padding(const Theme &theme) const;

    std::vector<std::unique_ptr<Component>> children_;
    Direction direction_;
    Align align_ = Align::Center;
    Justify justify_ = Justify::Start;
    float gap_ = 0.0f;
    float padding_ = 0.0f;
};

// Lays children out in rows of `columns` cells, filling left to right. Each column is as wide as
// its widest child, plus an equal share of any spare width; each row is as tall as its tallest
// child. Children stretch to fill their cell unless they set alignSelf.
// Gaps follow the same theme minimums as Container.
class Grid : public Container {
public:
    explicit Grid(int columns = 2);

    Grid &setColumns(int columns);

    Vec2 measure(const Theme &theme) const override;
    void arrange(const Rect &rect, const Theme &theme) override;

private:
    std::vector<float> columnWidths(const Theme &theme) const; // widest child in each column

    int columns_;
};

}
