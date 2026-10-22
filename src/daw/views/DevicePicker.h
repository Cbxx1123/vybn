#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "daw/library/DeviceCatalogue.h"
#include "ui/UI.h"

namespace daw {

struct DevicePickerOptions {
    bool instruments = true;  // instruments (a track's instrument) or effects (a chain's next)
    std::string title;        // "Instrument for Lead", "Add an effect to Lead"
    std::string current;      // the key of the device in use now (marked), if any
    ui::Rect anchor;          // opens beside this (below, or above when there's no room)
    std::function<void(const CatalogueEntry &entry)> onPick;
    std::function<void()> managePlugins; // "Manage plugins..." (Settings > Plugins)
    std::function<void()> browse;        // "Open the browser" (previews and downloads)
};

// The one place devices are chosen: a popover listing the catalogue's instruments or effects (the
// library's instruments and the built-in effects with the plugins installed, side by side) by
// category, with a search that has the keyboard as it opens. Up and Down move, Enter or a click
// chooses, Escape or a click outside closes. Opened from a chain's instrument card and its +, a
// track header's instrument line and a track's menu.
class DevicePicker : public ui::Component {
public:
    DevicePicker(ui::Context &context, DeviceCatalogue &catalogue, DevicePickerOptions options);
    ~DevicePicker() override;

    static DevicePicker &open(ui::Context &context, DeviceCatalogue &catalogue, DevicePickerOptions options);
    void close();

    ui::Vec2 measure(const ui::Theme &theme) const override;
    void arrange(const ui::Rect &window, const ui::Theme &theme) override;
    void handleInput(const ui::Input &input) override;
    void draw(ui::DrawList &list, const ui::Theme &theme) const override;
    bool hitTest(ui::Vec2 p) const override { return panel_->rect().contains(p); }

private:
    std::vector<const CatalogueEntry *> filtered();
    void rebuild();
    void requestRebuild();
    void move(int by);
    void pick(const CatalogueEntry &entry);

    ui::Context &context_;
    DeviceCatalogue &catalogue_;
    DevicePickerOptions options_;
    std::unique_ptr<ui::Panel> panel_;
    ui::TextField *search_ = nullptr;
    ui::ListView *categories_ = nullptr;
    ui::ScrollView *listScroll_ = nullptr;
    ui::ListView *list_ = nullptr;
    ui::Label *empty_ = nullptr;
    std::string category_; // empty for all
    std::vector<std::string> shownKeys_;
    int highlighted_ = 0;
    bool rebuildPosted_ = false;
    uint64_t builtFor_ = ~0ull; // the catalogue's version the lists show
    float shown_ = 0.0f;        // animated, 0 to 1
    bool focusedOnce_ = false;
    env::Connection keys_;
};

}
