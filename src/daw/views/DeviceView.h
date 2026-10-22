#pragma once

#include <memory>
#include <string>

#include "daw/views/DeviceControls.h"
#include "daw/views/PluginEditor.h"
#include "ui/UI.h"

namespace daw {

// A device's full view, the content of its own dock panel (floating at first, dockable anywhere):
// every parameter drawn by the app, by group with a search, for every kind of device; and for a
// plugin that can show its own interface inside the app, that interface one click away
// (Controls | Interface). The controls are the default, so every device looks like the rest of
// the app; the plugin's interface is there when its own layout is the better way in.
class DeviceView : public ui::Container {
public:
    // `title` and `caption` name it ("Surge XT", "Plugin instrument  /  Surge Synth Team"). A synth
    // (`synth`) opens on its own panel (Synth | All parameters) instead of the plugin pages.
    DeviceView(ui::Context &context, DeviceAccess access, std::string title, std::string caption, bool effect, bool synth = false);

    // The plugin's interface, once it has one (for the app to move away while it isn't shown)
    PluginEditorView *editor() { return editor_; }
    bool showingInterface() const { return pages_ && pages_->current() == interfaceIndex(); }
    void showInterface(bool interface);

    void handleInput(const ui::Input &input) override;

    // The most controls one page shows; a group or a search narrows a bigger device down
    static constexpr size_t kPageParams = 96;

private:
    // The pages' positions: a synth's own page comes first, then the controls, then the interface
    size_t controlsIndex() const { return synth_ ? 1 : 0; }
    size_t interfaceIndex() const { return synth_ ? 2 : 1; }
    plugin::Slot *slot() const;
    std::string headSignature() const;
    std::string controlsSignature() const;
    void rebuildHead();
    void rebuildControls();
    void request(std::string &built, std::string now, void (DeviceView::*rebuild)());

    ui::Context &context_;
    DeviceAccess access_;
    std::string title_, caption_;
    bool effect_;
    bool synth_;
    ui::Container *head_ = nullptr;
    ui::Pages *pages_ = nullptr;
    ui::ListView *groups_ = nullptr;
    ui::ScrollView *groupScroll_ = nullptr;
    ui::TextField *search_ = nullptr;
    ui::Container *grid_ = nullptr;
    ui::Container *interfacePage_ = nullptr;
    PluginEditorView *editor_ = nullptr;
    std::string group_; // the group shown (empty: all)
    std::string headBuilt_ = "?", controlsBuilt_ = "?";
    bool headPosted_ = false, controlsPosted_ = false;
    bool chosePage_ = false;     // the first page was picked (Interface for a plugin with nothing to show)
    bool wantInterface_ = false; // asked for the interface before it could show one
};

}
