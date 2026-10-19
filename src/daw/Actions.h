#pragma once

#include <string>

#include "ui/Keymap.h"

namespace daw {

// The DAW's keyboard actions, with their default keys, added to the keymap the UI layer's editing
// actions (edit.selectAll, copy, cut, paste, delete) are already in:
// - project.new/open/save/saveAs/settings, file.importAudio/importInstrument, app.settings,
//   app.quit
// - track.addAudio/addInstrument/delete, clip.duplicate/split
// - transport.playPause/stop/toStart/record, transport.metronome, view.follow
// - view.darkMode/mixer/editor/zoomIn/zoomOut, help.shortcuts
// - notes.up/down/octaveUp/octaveDown/earlier/later/deselect (the piano roll's selected note)
// Menu items and views hook them by id; the shortcuts editor changes their keys.
void defineActions(ui::Keymap &keymap);

// The user's changed shortcuts, kept in %APPDATA%\Vibin\keybinds.toml beside settings.ini: a
// [keys] table of action id to a list of chords ("project.save" = ["Ctrl+S", "F2"]; [] for
// none). Only actions whose keys differ from the defaults are written. Loading applies them;
// a missing file is fine, a broken one is reported and ignored, and chords that don't parse
// are skipped (and reported).
std::string keybindsPath();
bool loadKeybinds(ui::Keymap &keymap, std::string &error);
bool saveKeybinds(const ui::Keymap &keymap, std::string &error);
// The same, through text, for tests
std::string writeKeybinds(const ui::Keymap &keymap);
bool readKeybinds(const std::string &text, ui::Keymap &keymap, std::string &error);

}
