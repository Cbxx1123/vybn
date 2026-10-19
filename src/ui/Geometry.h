#pragma once

namespace ui {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// Pixel rectangle; origin at the top-left of the window, y pointing down
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(Vec2 p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }
    Rect inset(float amount) const { return {x + amount, y + amount, w - 2.0f * amount, h - 2.0f * amount}; }
};

// Layout vocabulary, shared by components and containers
enum class Direction { Row, Column };
enum class Align { Start, Center, End, Stretch };       // cross axis
enum class Justify { Start, Center, End, SpaceBetween }; // main axis

}
