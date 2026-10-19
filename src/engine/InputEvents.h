#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace engine {

// Keys by their position on a US keyboard, as GLFW names them
enum class Key : uint16_t {
    Unknown,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Space, Enter, Escape, Tab, Backspace, Delete, Insert,
    Left, Right, Up, Down, Home, End, PageUp, PageDown,
    Minus, Equal, LeftBracket, RightBracket, Backslash, Semicolon, Apostrophe, Comma, Period, Slash, Grave,
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4, Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal, KeypadDivide, KeypadMultiply, KeypadSubtract, KeypadAdd,
    // The modifiers themselves, which arrive as keys too
    Shift, Ctrl, Alt, Super,
    CapsLock, PrintScreen, Pause, Menu,
    Count,
};

// Modifier bits
enum Mod : uint8_t {
    ModShift = 1,
    ModCtrl = 2,
    ModAlt = 4,
    ModSuper = 8,
};

bool isModifier(Key key);
// "A", "5", "F5", "Space", "Left", "-", "Keypad 4"... (empty for Unknown)
std::string keyName(Key key);
// The reverse of keyName, ignoring case; Unknown if it isn't one
Key keyFromName(const std::string &name);

// A key with the modifiers held, as shortcuts are written: "Ctrl+Shift+S", "Space", "Ctrl+-"
struct Chord {
    Key key = Key::Unknown;
    uint8_t mods = 0;

    bool empty() const { return key == Key::Unknown; }
    std::string toString() const;
    // Parses toString's form (modifiers in any order and case; "Cmd" and "Win" mean Super).
    static std::optional<Chord> parse(const std::string &text);

    bool operator==(const Chord &o) const { return key == o.key && mods == o.mods; }
    bool operator!=(const Chord &o) const { return !(*this == o); }
};

// Input from a window, in the order it happened. Positions are window coordinates.
struct KeyEvent {
    Key key = Key::Unknown;
    uint8_t mods = 0;
    bool down = true;    // false for a release
    bool repeat = false; // held down long enough to repeat

    Chord chord() const { return {key, mods}; }
};

struct TextEvent {
    uint32_t codepoint = 0; // a Unicode character typed
};

// The mouse pointer's shape over a window: what pressing there will do
enum class Cursor : uint8_t { Arrow, Hand, Crosshair, IBeam, ResizeX, ResizeY, Move, NotAllowed };

enum class MouseButton : uint8_t { Left, Right, Middle, Back, Forward };

struct MouseButtonEvent {
    MouseButton button = MouseButton::Left;
    bool down = true;
    uint8_t mods = 0;
    double x = 0.0, y = 0.0;
};

struct MouseMoveEvent {
    double x = 0.0, y = 0.0;
};

struct ScrollEvent {
    double x = 0.0, y = 0.0; // lines; positive y is away from the user, positive x is to the left
    uint8_t mods = 0;
};

using InputEvent = std::variant<KeyEvent, TextEvent, MouseButtonEvent, MouseMoveEvent, ScrollEvent>;

}
