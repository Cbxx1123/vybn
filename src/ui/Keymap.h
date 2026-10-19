#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/InputEvents.h"

namespace ui {

using engine::Chord;

// Something the user can do from the keyboard, named so its keys can be changed: "project.save",
// "edit.selectAll". Its handlers hook it by id on InputSignals; the Keymap only says which keys
// run it.
struct ActionInfo {
    std::string id;
    std::string label;    // for menus of shortcuts: "Save as..."
    std::string category; // "Project", "Editing"
    std::vector<Chord> defaults;
    bool repeats = false; // whether holding the keys runs it again (arrows, delete)
};

// Which keys run which actions. Each action has any number of chords, starting from its
// defaults; changed ones are remembered as overrides, which is what gets saved. One map is
// shared by every window. A chord may run several actions (the same chord in different places);
// they are offered in the order they were defined until one is handled.
//
// The UI layer defines its own editing actions in the constructor, which text fields hook:
// edit.selectAll (Ctrl+A), edit.copy (Ctrl+C), edit.cut (Ctrl+X), edit.paste (Ctrl+V) and
// edit.delete (Delete, Backspace).
class Keymap {
public:
    Keymap();

    // Adds an action, or updates its label and defaults. Chords given to setOverrides before
    // it was defined apply now.
    void define(ActionInfo info);
    const std::vector<ActionInfo> &actions() const { return infos_; }
    const ActionInfo *find(const std::string &id) const;

    const std::vector<Chord> &chords(const std::string &id) const;
    // The first chord as text, for showing beside a menu item; empty if it has none
    std::string shortcutText(const std::string &id) const;
    std::vector<std::string> actionsFor(const Chord &chord) const;

    // Gives the action these chords instead of its current ones
    void setChords(const std::string &id, std::vector<Chord> chords);
    // Binds chord to the action (in place of its chord at `replace`, or added when -1), taking it
    // from every other action that had it. Returns the labels of those actions.
    std::vector<std::string> assign(const std::string &id, const Chord &chord, int replace = -1);
    void removeChord(const std::string &id, const Chord &chord);
    void reset(const std::string &id);
    void resetAll();
    bool isDefault(const std::string &id) const;

    // The actions whose chords differ from their defaults, for saving
    std::vector<std::pair<std::string, std::vector<Chord>>> overrides() const;
    // Applies saved overrides; ones for actions not defined yet wait for define
    void setOverrides(const std::vector<std::pair<std::string, std::vector<Chord>>> &overrides);

    // Goes up on every change, so views of the bindings know to refresh
    uint64_t version() const { return version_; }

private:
    int indexOf(const std::string &id) const;

    std::vector<ActionInfo> infos_;
    std::vector<std::vector<Chord>> chords_; // per action, parallel to infos_
    std::map<std::string, std::vector<Chord>> pending_;
    uint64_t version_ = 1;
};

}
