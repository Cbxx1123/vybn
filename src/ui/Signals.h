#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Keymap.h"
#include "engine/InputEvents.h"
#include "env/Signal.h"

namespace ui {

// Where a hook stands in line. Higher goes first; among equals, the newest hook goes first.
namespace Priority {
constexpr int App = 0;          // application commands (the menu bar's actions)
constexpr int Component = 100;  // a view's own keys (the piano roll's arrows)
constexpr int Modal = 200;      // a dialog, which keeps keys from what is under it
constexpr int Popup = 300;      // an open menu, which takes every key while open
constexpr int Focus = 400;      // the focused text field
constexpr int Capture = 1000;   // waiting for the next key (choosing a shortcut)
}

// An action being run: from its keys, a menu, or anything that calls trigger
struct ActionEvent {
    std::string id;
    bool repeat = false; // from a held key
};

// One window's input, as signals anything can hook. Context feeds it the window's events each
// frame: mouse and wheel first (before the UI tree sees the mouse), then the keyboard in the
// order it was typed. A key goes to the key signal; if no hook handles a press, the Keymap
// turns it into actions, offered to the action signal one by one until one is handled. Typed
// characters go to the text signal. Actions nobody here handles go on to the fallback (the main
// window's signals), so the app's commands work from every window.
//
// It also holds the keyboard focus: one owner (a text field) at a time, cleared when a dialog
// or menu opens.
class InputSignals {
public:
    InputSignals() : id_(nextId()) {}
    InputSignals(const InputSignals &) = delete;
    InputSignals &operator=(const InputSignals &) = delete;

    // Different for every InputSignals ever made, so a hook can tell a new window's signals from
    // a closed one's at the same address
    uint64_t id() const { return id_; }

    env::Signal<const engine::KeyEvent &> key;
    env::Signal<const engine::TextEvent &> text;
    env::Signal<const engine::MouseButtonEvent &> mouseButton; // framebuffer pixels
    env::Signal<const engine::ScrollEvent &> scroll;
    env::Signal<const ActionEvent &> action;

    // Hooks one action by id
    [[nodiscard]] env::Connection onAction(std::string id, std::function<bool(const ActionEvent &)> handler,
                                           int priority = Priority::App);
    // Runs an action as if its keys were pressed: for menus, MIDI controllers, scripts. Returns
    // whether something handled it.
    bool trigger(const std::string &id, bool repeat = false);

    // Feeding, from Context (or a test)
    bool dispatch(const engine::KeyEvent &event);
    bool dispatch(const engine::TextEvent &event);

    void setKeymap(Keymap *keymap) { keymap_ = keymap ? keymap : &own_; }
    Keymap &keymap() { return *keymap_; }
    const Keymap &keymap() const { return *keymap_; }
    void setFallback(InputSignals *fallback) { fallback_ = fallback; }

    void setFocus(const void *owner) { focus_ = owner; }
    const void *focus() const { return focus_; }

    // Counts frames, so hooks can tell whether their component is still being shown
    uint64_t frame() const { return frame_; }
    void beginFrame() { ++frame_; }

    // The system clipboard, through the window (empty and ignored without one)
    std::function<std::string()> readClipboard;
    std::function<void(const std::string &)> writeClipboard;
    std::string clipboard() const { return readClipboard ? readClipboard() : std::string(); }
    void copy(const std::string &text) const
    {
        if (writeClipboard)
            writeClipboard(text);
    }

private:
    static uint64_t nextId()
    {
        static uint64_t next = 0;
        return ++next;
    }

    uint64_t id_;
    Keymap own_;
    Keymap *keymap_ = &own_;
    InputSignals *fallback_ = nullptr;
    const void *focus_ = nullptr;
    uint64_t frame_ = 0;
};

// A component's hooks on its window's signals. A component makes them the first time it gets
// input and again whenever it has moved to another window:
//
//     void handleInput(const Input &input) {
//         if (hooks_.attach(input.signals))
//             hooks_.key(Priority::Component, [this](const KeyEvent &e) { ... });
//
// Every hook made through it runs only while the component is live: it got input this frame
// (hidden pages, closed drawers and components no longer in the tree don't). Destroying the
// Hooks (with its component) removes them.
class Hooks {
public:
    Hooks() = default;
    Hooks(const Hooks &) = delete; // the hooks point at it
    Hooks &operator=(const Hooks &) = delete;

    // Marks the component live this frame. Returns true when hooks should be made: the first
    // time, or after a move to another window's signals (the old hooks are gone by then). With
    // no signals (input that isn't for this component, such as a closed drawer's), it does
    // nothing and the component isn't live.
    bool attach(InputSignals *signals);
    InputSignals *signals() const { return signals_; }
    bool live() const { return signals_ && seen_ == signals_->frame(); }

    void key(int priority, std::function<bool(const engine::KeyEvent &)> handler);
    void text(int priority, std::function<bool(const engine::TextEvent &)> handler);
    void action(const std::string &id, int priority, std::function<bool(const ActionEvent &)> handler);
    // Every action, whatever its id (a menu or dialog keeping them from what is under it)
    void anyAction(int priority, std::function<bool(const ActionEvent &)> handler);
    void clear();

private:
    InputSignals *signals_ = nullptr;
    uint64_t signalsId_ = 0;
    uint64_t seen_ = 0;
    std::vector<env::Connection> connections_;
};

}
