#include "Layout.h"

#include <algorithm>

namespace ui {

Container::Container(Direction direction)
    : direction_(direction)
{
}

std::unique_ptr<Component> Container::release(const Component &child)
{
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->get() == &child) {
            std::unique_ptr<Component> released = std::move(*it);
            children_.erase(it);
            treeChanged();
            return released;
        }
    }
    return nullptr;
}

Container &Container::setDirection(Direction direction)
{
    direction_ = direction;
    return *this;
}

Container &Container::setAlign(Align align)
{
    align_ = align;
    return *this;
}

Container &Container::setJustify(Justify justify)
{
    justify_ = justify;
    return *this;
}

Container &Container::setGap(float gap)
{
    gap_ = gap;
    return *this;
}

Container &Container::setPadding(float padding)
{
    padding_ = padding;
    return *this;
}

float Container::gap(const Theme &theme) const
{
    return std::max(theme.spacing(), gap_);
}

float Container::padding(const Theme &) const
{
    return padding_;
}

Vec2 Container::measure(const Theme &theme) const
{
    const bool row = direction_ == Direction::Row;
    float main = 0.0f;
    float cross = 0.0f;
    for (const auto &child : children_) {
        Vec2 size = child->preferredSize(theme);
        main += row ? size.x : size.y;
        cross = std::max(cross, row ? size.y : size.x);
    }
    if (!children_.empty())
        main += gap(theme) * static_cast<float>(children_.size() - 1);

    float pad = 2.0f * padding(theme);
    return row ? Vec2{main + pad, cross + pad} : Vec2{cross + pad, main + pad};
}

void Container::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    if (children_.empty())
        return;

    const bool row = direction_ == Direction::Row;
    const float pad = padding(theme);
    const float gapSize = gap(theme);
    const Rect content{rect.x + pad, rect.y + pad, std::max(0.0f, rect.w - 2.0f * pad), std::max(0.0f, rect.h - 2.0f * pad)};
    const float mainSpace = row ? content.w : content.h;
    const float crossSpace = row ? content.h : content.w;

    std::vector<Vec2> sizes;
    sizes.reserve(children_.size());
    float used = gapSize * static_cast<float>(children_.size() - 1);
    float totalGrow = 0.0f;
    for (const auto &child : children_) {
        sizes.push_back(child->preferredSize(theme));
        used += row ? sizes.back().x : sizes.back().y;
        totalGrow += child->grow();
    }
    const float extra = std::max(0.0f, mainSpace - used);

    // Leftover space goes to growing children, or else is placed by justify
    float cursor = 0.0f;
    float between = gapSize;
    if (totalGrow <= 0.0f) {
        switch (justify_) {
        case Justify::Center:
            cursor = extra * 0.5f;
            break;
        case Justify::End:
            cursor = extra;
            break;
        case Justify::SpaceBetween:
            if (children_.size() > 1)
                between += extra / static_cast<float>(children_.size() - 1);
            break;
        case Justify::Start:
            break;
        }
    }

    for (size_t i = 0; i < children_.size(); ++i) {
        Component &child = *children_[i];
        float main = row ? sizes[i].x : sizes[i].y;
        if (totalGrow > 0.0f)
            main += extra * child.grow() / totalGrow;

        float cross = row ? sizes[i].y : sizes[i].x;
        float crossPos = 0.0f;
        switch (child.alignSelf().value_or(align_)) {
        case Align::Stretch:
            cross = crossSpace;
            break;
        case Align::Center:
            crossPos = (crossSpace - cross) * 0.5f;
            break;
        case Align::End:
            crossPos = crossSpace - cross;
            break;
        case Align::Start:
            break;
        }

        Rect childRect = row ? Rect{content.x + cursor, content.y + crossPos, main, cross}
                             : Rect{content.x + crossPos, content.y + cursor, cross, main};
        child.arrange(childRect, theme);
        cursor += main + between;
    }
}

void Container::handleInput(const Input &input)
{
    for (auto &child : children_)
        child->handleInput(input);
}

void Container::draw(DrawList &list, const Theme &theme) const
{
    for (const auto &child : children_)
        child->draw(list, theme);
}

// Grid

Grid::Grid(int columns)
    : Container(Direction::Row), columns_(std::max(1, columns))
{
    align_ = Align::Stretch;
}

Grid &Grid::setColumns(int columns)
{
    columns_ = std::max(1, columns);
    return *this;
}

std::vector<float> Grid::columnWidths(const Theme &theme) const
{
    std::vector<float> widths(static_cast<size_t>(columns_), 0.0f);
    for (size_t i = 0; i < children_.size(); ++i)
        widths[i % widths.size()] = std::max(widths[i % widths.size()], children_[i]->preferredSize(theme).x);
    return widths;
}

Vec2 Grid::measure(const Theme &theme) const
{
    const size_t columns = static_cast<size_t>(columns_);
    const float gapSize = gap(theme);
    float height = 0.0f;
    float rowHeight = 0.0f;
    size_t rows = 0;
    for (size_t i = 0; i < children_.size(); ++i) {
        rowHeight = std::max(rowHeight, children_[i]->preferredSize(theme).y);
        if (i % columns == columns - 1 || i + 1 == children_.size()) {
            height += rowHeight;
            rowHeight = 0.0f;
            ++rows;
        }
    }
    float width = 0.0f;
    for (float w : columnWidths(theme))
        width += w;
    const size_t used = std::min(columns, children_.size());
    const float pad = 2.0f * padding(theme);
    width += gapSize * (used > 0 ? used - 1 : 0);
    height += gapSize * (rows > 0 ? rows - 1 : 0);
    return {width + pad, height + pad};
}

void Grid::arrange(const Rect &rect, const Theme &theme)
{
    rect_ = rect;
    if (children_.empty())
        return;

    const size_t columns = static_cast<size_t>(columns_);
    const float pad = padding(theme);
    const float gapSize = gap(theme);

    // Each column starts at its widest child; spare room is shared equally, and a shortfall
    // shrinks columns in proportion
    std::vector<float> widths = columnWidths(theme);
    float wanted = 0.0f;
    for (float w : widths)
        wanted += w;
    const float available = std::max(0.0f, rect.w - 2.0f * pad - gapSize * (columns - 1));
    for (float &w : widths) {
        if (wanted <= 0.0f)
            w = available / columns;
        else if (available >= wanted)
            w += (available - wanted) / columns;
        else
            w *= available / wanted;
    }

    float y = rect.y + pad;
    for (size_t rowStart = 0; rowStart < children_.size(); rowStart += columns) {
        const size_t rowEnd = std::min(rowStart + columns, children_.size());
        float rowHeight = 0.0f;
        for (size_t i = rowStart; i < rowEnd; ++i)
            rowHeight = std::max(rowHeight, children_[i]->preferredSize(theme).y);

        float x = rect.x + pad;
        for (size_t i = rowStart; i < rowEnd; ++i) {
            Component &child = *children_[i];
            const float cellWidth = widths[i - rowStart];
            const Vec2 size = child.preferredSize(theme);
            const float w = std::min(size.x, cellWidth);
            Rect cell{x, y, cellWidth, rowHeight};
            switch (child.alignSelf().value_or(align_)) {
            case Align::Start:
                cell = {x, y, w, size.y};
                break;
            case Align::Center:
                cell = {x + (cellWidth - w) * 0.5f, y + (rowHeight - size.y) * 0.5f, w, size.y};
                break;
            case Align::End:
                cell = {x + cellWidth - w, y + rowHeight - size.y, w, size.y};
                break;
            case Align::Stretch:
                break;
            }
            child.arrange(cell, theme);
            x += cellWidth + gapSize;
        }
        y += rowHeight + gapSize;
    }
}

}
