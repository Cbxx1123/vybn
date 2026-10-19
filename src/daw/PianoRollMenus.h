#pragma once

#include <vector>

#include "Arpeggiator.h"
#include "NoteTools.h"
#include "ui/UI.h"

namespace env {
class Environment;
}
namespace daw {

class PianoRollView;

// The piano roll's menus: the note tools (the Notes menu, also its right-click menu), the Insert menu
// (scale runs and chords), the toolbar's grid, scale and View menus, what the lane shows, and the
// arpeggiator's dialog with its last settings. Each returns its items when a menu opens, reading the
// roll as it is then, so the menus follow the roll's state. The roll and the context must outlive it.
class PianoRollMenus {
public:
    PianoRollMenus(env::Environment &env, ui::Context &gui, PianoRollView &roll);

    PianoRollMenus(const PianoRollMenus &) = delete;
    PianoRollMenus &operator=(const PianoRollMenus &) = delete;

    std::vector<ui::MenuItem> noteMenu();
    std::vector<ui::MenuItem> insertMenu();
    std::vector<ui::MenuItem> gridMenu();
    std::vector<ui::MenuItem> scaleMenu();
    std::vector<ui::MenuItem> rollViewMenu();
    std::vector<ui::MenuItem> laneMenu();
    // The arpeggiator's dialog, on the clip's notes (nothing without a clip)
    void openArpeggiator();

private:
    env::Environment &env_;
    ui::Context &gui_;
    PianoRollView &roll_;
    ArpSettings arp_; // the arpeggiator's last settings
};

}
