#include "daw/app/Actions.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include "daw/app/Settings.h"
#include "daw/io/Toml.h"

namespace daw {

namespace fs = std::filesystem;
using engine::Key;
using engine::ModCtrl;
using engine::ModShift;
using ui::Chord;

void defineActions(ui::Keymap &keymap)
{
    const uint8_t ctrl = ModCtrl;
    const uint8_t ctrlShift = ModCtrl | ModShift;
    const uint8_t ctrlAlt = ModCtrl | engine::ModAlt;
    auto define = [&keymap](const char *id, const char *label, const char *category, std::vector<Chord> keys,
                            bool repeats = false) { keymap.define({id, label, category, std::move(keys), repeats}); };

    define("project.new", "New project", "Project", {{Key::N, ctrl}});
    define("project.open", "Open project", "Project", {{Key::O, ctrl}});
    define("project.save", "Save", "Project", {{Key::S, ctrl}});
    define("project.saveAs", "Save as", "Project", {{Key::S, ctrlShift}});
    define("project.settings", "Project settings", "Project", {{Key::P, ctrlShift}});
    define("project.export", "Export...", "Project", {{Key::E, ctrlShift}});
    define("project.saveVersion", "Save version", "Project", {{Key::S, ctrlAlt}});
    define("project.versions", "Versions", "Project", {});
    define("file.importAudio", "Import audio file", "Project", {{Key::I, ctrlShift}});
    define("file.importInstrument", "Import instrument", "Project", {{Key::I, ctrl}});
    define("app.settings", "Settings", "Project", {{Key::Comma, ctrl}});
    define("app.quit", "Quit", "Project", {{Key::Q, ctrl}});

    define("edit.undo", "Undo", "Editing", {{Key::Z, ctrl}}, true);
    define("edit.redo", "Redo", "Editing", {{Key::Z, ctrlShift}, {Key::Y, ctrl}}, true);

    define("track.addAudio", "Add audio track", "Tracks and clips", {{Key::T, ctrl}});
    define("track.addInstrument", "Add instrument track", "Tracks and clips", {{Key::T, ctrlShift}});
    define("track.addReturn", "Add return track", "Tracks and clips", {{Key::T, ctrlAlt}});
    define("track.addGroup", "Add group track", "Tracks and clips", {});
    define("track.group", "Group the track", "Tracks and clips", {{Key::G, ctrl}});
    define("track.delete", "Delete track", "Tracks and clips", {{Key::Delete, ctrl}});
    define("clip.duplicate", "Duplicate clip", "Tracks and clips", {{Key::D, ctrl}});
    define("clip.split", "Split clip at playhead", "Tracks and clips", {{Key::E, ctrl}});

    define("transport.playPause", "Play / pause", "Transport", {{Key::Space, 0}});
    define("transport.stop", "Stop", "Transport", {});
    define("transport.toStart", "Return to start", "Transport", {{Key::Home, 0}});
    define("transport.record", "Record", "Transport", {{Key::R, ctrl}});
    define("transport.metronome", "Metronome", "Transport", {{Key::K, ctrl}});
    define("transport.loop", "Loop", "Transport", {{Key::L, ctrl}});
    define("view.follow", "Follow playhead", "Transport", {{Key::F, ctrl}});
    define("transport.touchAutomation", "Touch automation", "Transport", {});

    define("view.darkMode", "Dark mode", "View", {{Key::D, ctrlShift}});
    define("view.appearance", "Appearance", "View", {});
    define("view.foldAll", "Fold all groups", "View", {});
    define("view.unfoldAll", "Unfold all groups", "View", {});
    define("view.timing", "Tempo and signature lanes", "View", {{Key::M, ctrlShift}});
    define("view.mixer", "Show the mixer", "View", {{Key::Num1, ctrl}});
    define("view.editor", "Show the clip editor", "View", {{Key::Num2, ctrl}});
    define("view.effects", "Show the devices", "View", {{Key::Num3, ctrl}});
    define("view.exports", "Show the exports", "View", {{Key::Num4, ctrl}});
    define("view.steps", "Step sequencer", "View", {{Key::Num5, ctrl}});
    define("view.browser", "Browser", "View", {{Key::Tab, 0}});
    define("view.zoomIn", "Zoom in", "View", {{Key::Equal, ctrl}}, true);
    define("view.zoomOut", "Zoom out", "View", {{Key::Minus, ctrl}}, true);
    define("help.shortcuts", "Keyboard shortcuts", "View", {{Key::F1, 0}});

    define("notes.up", "Note up a semitone", "Piano roll", {{Key::Up, 0}}, true);
    define("notes.down", "Note down a semitone", "Piano roll", {{Key::Down, 0}}, true);
    define("notes.octaveUp", "Note up an octave", "Piano roll", {{Key::Up, ModShift}}, true);
    define("notes.octaveDown", "Note down an octave", "Piano roll", {{Key::Down, ModShift}}, true);
    define("notes.earlier", "Note earlier", "Piano roll", {{Key::Left, 0}}, true);
    define("notes.later", "Note later", "Piano roll", {{Key::Right, 0}}, true);
    define("notes.deselect", "Deselect the notes", "Piano roll", {{Key::Escape, 0}});
    define("notes.louder", "Notes louder", "Piano roll", {{Key::Up, ctrl}}, true);
    define("notes.softer", "Notes softer", "Piano roll", {{Key::Down, ctrl}}, true);
    define("notes.quantize", "Quantize notes", "Piano roll", {{Key::Q, 0}});
    define("notes.drawTool", "Draw tool", "Piano roll", {{Key::B, 0}});
    define("notes.selectTool", "Select tool", "Piano roll", {{Key::V, 0}});
    define("notes.sliceTool", "Slice tool", "Piano roll", {{Key::C, 0}});
    define("notes.eraseTool", "Erase tool", "Piano roll", {{Key::E, 0}});
    define("notes.nudgeEarlier", "Nudge notes earlier", "Piano roll", {{Key::Left, engine::ModAlt}}, true);
    define("notes.nudgeLater", "Nudge notes later", "Piano roll", {{Key::Right, engine::ModAlt}}, true);
    define("notes.shorter", "Notes shorter", "Piano roll", {{Key::Left, ModShift}}, true);
    define("notes.longer", "Notes longer", "Piano roll", {{Key::Right, ModShift}}, true);
    define("notes.previous", "Select the previous note", "Piano roll", {{Key::Left, ctrl}}, true);
    define("notes.next", "Select the next note", "Piano roll", {{Key::Right, ctrl}}, true);
    define("notes.mute", "Mute or unmute notes", "Piano roll", {{Key::M, 0}});
    define("notes.loop", "Loop the selected notes", "Piano roll", {{Key::L, 0}});
    define("notes.fit", "Zoom to fit the notes", "Piano roll", {{Key::F, 0}});
}

std::string keybindsPath()
{
    return (fs::path(Settings::filePath()).parent_path() / "keybinds.toml").string();
}

std::string writeKeybinds(const ui::Keymap &keymap)
{
    std::ostringstream out;
    out << "# VYBN keyboard shortcuts: the ones changed from the defaults.\n";
    out << "# Each action lists its keys, such as \"Ctrl+Shift+S\" or \"Space\"; [] means none.\n";
    out << "[keys]\n";
    for (const auto &[id, chords] : keymap.overrides()) {
        out << toml::key(id) << " = [";
        for (size_t i = 0; i < chords.size(); ++i)
            out << (i ? ", " : "") << toml::quote(chords[i].toString());
        out << "]\n";
    }
    return out.str();
}

bool readKeybinds(const std::string &text, ui::Keymap &keymap, std::string &error)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return false;
    error.clear();
    const toml::Value *keys = root.at("keys");
    if (!keys)
        return true;
    if (!keys->isTable()) {
        error = "keys should be a table";
        return false;
    }
    std::vector<std::pair<std::string, std::vector<Chord>>> overrides;
    std::string skipped;
    for (const auto &[id, value] : keys->table().entries()) {
        if (!value.isArray()) {
            skipped += (skipped.empty() ? "" : ", ") + id;
            continue;
        }
        std::vector<Chord> chords;
        for (const toml::Value &item : value.array()) {
            std::optional<Chord> chord = item.type() == toml::Type::String ? Chord::parse(item.string()) : std::nullopt;
            if (chord)
                chords.push_back(*chord);
            else
                skipped += (skipped.empty() ? "" : ", ") + id + " " +
                           (item.type() == toml::Type::String ? "\"" + item.string() + "\"" : std::string("(not text)"));
        }
        overrides.emplace_back(id, std::move(chords));
    }
    keymap.setOverrides(overrides);
    if (!skipped.empty())
        error = "skipped keys it couldn't read: " + skipped;
    return skipped.empty();
}

bool loadKeybinds(ui::Keymap &keymap, std::string &error)
{
    std::ifstream in(fs::path(keybindsPath()), std::ios::binary);
    if (!in)
        return true; // nothing changed yet
    std::ostringstream text;
    text << in.rdbuf();
    if (!readKeybinds(text.str(), keymap, error)) {
        error = "keybinds.toml, " + error;
        return false;
    }
    return true;
}

bool saveKeybinds(const ui::Keymap &keymap, std::string &error)
{
    const fs::path path(keybindsPath());
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::string text = writeKeybinds(keymap);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) {
        error = "Couldn't write " + path.string();
        return false;
    }
    return true;
}

}
