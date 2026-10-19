#include "InputEvents.h"

#include <cctype>
#include <vector>

namespace engine {

namespace {

const char *const kNames[] = {
    "",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Space", "Enter", "Escape", "Tab", "Backspace", "Delete", "Insert",
    "Left", "Right", "Up", "Down", "Home", "End", "Page Up", "Page Down",
    "-", "=", "[", "]", "\\", ";", "'", ",", ".", "/", "`",
    "Keypad 0", "Keypad 1", "Keypad 2", "Keypad 3", "Keypad 4", "Keypad 5", "Keypad 6", "Keypad 7", "Keypad 8",
    "Keypad 9",
    "Keypad .", "Keypad /", "Keypad *", "Keypad -", "Keypad +",
    "Shift", "Ctrl", "Alt", "Super",
    "Caps Lock", "Print Screen", "Pause", "Menu",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(Key::Count), "a name for every key");

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}

bool isModifier(Key key)
{
    return key == Key::Shift || key == Key::Ctrl || key == Key::Alt || key == Key::Super;
}

std::string keyName(Key key)
{
    const size_t i = static_cast<size_t>(key);
    return i < static_cast<size_t>(Key::Count) ? kNames[i] : "";
}

Key keyFromName(const std::string &name)
{
    const std::string wanted = lower(name);
    if (wanted.empty())
        return Key::Unknown;
    for (size_t i = 1; i < static_cast<size_t>(Key::Count); ++i) {
        if (lower(kNames[i]) == wanted)
            return static_cast<Key>(i);
    }
    // Other spellings people write
    if (wanted == "esc")
        return Key::Escape;
    if (wanted == "return")
        return Key::Enter;
    if (wanted == "del")
        return Key::Delete;
    if (wanted == "ins")
        return Key::Insert;
    if (wanted == "pageup" || wanted == "pgup")
        return Key::PageUp;
    if (wanted == "pagedown" || wanted == "pgdn")
        return Key::PageDown;
    if (wanted == "minus")
        return Key::Minus;
    if (wanted == "equal" || wanted == "equals")
        return Key::Equal;
    return Key::Unknown;
}

std::string Chord::toString() const
{
    if (empty())
        return {};
    std::string s;
    if (mods & ModCtrl)
        s += "Ctrl+";
    if (mods & ModAlt)
        s += "Alt+";
    if (mods & ModShift)
        s += "Shift+";
    if (mods & ModSuper)
        s += "Super+";
    return s + keyName(key);
}

std::optional<Chord> Chord::parse(const std::string &text)
{
    // Split on '+'. A '+' straight after another is kept as text, so "Ctrl++" ends in "+", which
    // isn't a key name and is refused: the keypad's is written "Keypad +".
    std::vector<std::string> parts;
    std::string part;
    for (char c : text) {
        if (c == '+' && !part.empty()) {
            parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    parts.push_back(part);
    // "Keypad +" split into "Keypad " and ""
    if (parts.size() >= 2 && parts.back().empty()) {
        parts.pop_back();
        parts.back() += '+';
    }

    Chord chord;
    for (size_t i = 0; i < parts.size(); ++i) {
        // Trim spaces, but "Keypad 4" keeps its inner one
        std::string p = parts[i];
        while (!p.empty() && p.front() == ' ')
            p.erase(0, 1);
        while (!p.empty() && p.back() == ' ')
            p.pop_back();
        const std::string l = lower(p);
        const bool last = i + 1 == parts.size();
        if (!last) {
            if (l == "ctrl" || l == "control")
                chord.mods |= ModCtrl;
            else if (l == "alt" || l == "option")
                chord.mods |= ModAlt;
            else if (l == "shift")
                chord.mods |= ModShift;
            else if (l == "super" || l == "cmd" || l == "win" || l == "meta")
                chord.mods |= ModSuper;
            else
                return std::nullopt;
            continue;
        }
        chord.key = keyFromName(p);
        if (chord.key == Key::Unknown || isModifier(chord.key))
            return std::nullopt;
    }
    return chord;
}

}
