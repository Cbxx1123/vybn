#pragma once

#include <functional>
#include <string>
#include <vector>

#include "plugin/Slot.h"
#include "ui/UI.h"

namespace daw {

// A plugin's own interface inside the app: the pictures its sandboxed host captures of it, drawn by
// the UI renderer pixel for pixel in an inset well, with the mouse and keys over it sent back to
// the plugin. The plugin's changes flow to the audio and the project as they do from its own
// window (its parameter changes come back from its processing). Each frame it tells the plugin's
// host where the picture is on the desktop, so the plugin's own (invisible, click-through) window
// sits under it and the menus and dialogs it opens appear in the right place, to be used
// directly; hideIfUnseen moves it away while the view isn't shown. It finds its plugin through
// `slot` each frame, so a reload (or the effect going) is followed: a slot that is ready again
// gets its interface opened again. Clicking it takes the window's keyboard focus (keys then go to
// the plugin until Escape or a click elsewhere).
class PluginEditorView : public ui::Component {
public:
    PluginEditorView(std::function<plugin::Slot *()> slot, std::string name);

    // The size of the plugin's picture (0 until the first arrives), for sizing its window
    uint32_t pictureWidth() const { return image_.width; }
    uint32_t pictureHeight() const { return image_.height; }
    // Once a frame from outside: if it wasn't shown lately (its panel hidden, its window
    // minimised), the plugin's window is moved away
    void hideIfUnseen(double now);

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;

private:
    ui::Rect pictureRect() const;
    void makeHooks();
    bool focused() const;
    void send(plugin::GuiInputKind kind, int32_t x, int32_t y, uint32_t button = 0, int32_t wheel = 0, uint32_t key = 0);

    std::function<plugin::Slot *()> slot_;
    std::string name_;
    ui::ImageData image_;
    uint32_t sequence_ = 0;
    ui::Hooks hooks_;
    std::string status_; // shown until a picture arrives
    double lastAsk_ = -10.0;
    bool inside_ = false;
    bool dragging_ = false;
    bool middleHeld_ = false;
    double lastPress_ = -10.0;
    int32_t lastX_ = -1, lastY_ = -1;
    uint8_t mods_ = 0;
    double lastSeen_ = -10.0;
    bool placed_ = false; // the host has been told where it is shown
    int32_t placedX_ = 0, placedY_ = 0;
    void *placedWindow_ = nullptr;
    double placedAt_ = -10.0;
    double allowedAt_ = -10.0; // when the plugin's host was last allowed to the front
    int32_t clip_[4] = {-1, -1, -1, -1}; // the part of the picture last said to be shown
};

// engine::Key as a Windows virtual key (0 for none)
uint32_t virtualKey(engine::Key key);

}
