#include "ProjectSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "audio/Effects.h"
#include "audio/PluginInstrument.h"
#include "audio/Synth.h"
#include "daw/Appearance.h"
#include "daw/Autosave.h"
#include "daw/ExportModel.h"
#include "daw/ExportModel.h"
#include "daw/History.h"
#include "daw/ProjectFile.h"
#include "daw/ProjectSession.h"
#include "daw/ProjectStore.h"
#include "daw/Toml.h"

namespace tests {

namespace {

struct Checker {
    int failures = 0;

    void check(bool ok, const std::string &what, const std::string &detail = {})
    {
        std::printf("%s  %-44s %s\n", ok ? "PASS" : "FAIL", what.c_str(), detail.c_str());
        if (!ok)
            ++failures;
    }
};

// Reads text that should be refused; returns the error (empty if it was accepted)
std::string refusal(const std::string &text)
{
    daw::Project p;
    std::string error;
    return daw::readProject(text, p, error) ? std::string() : error;
}

size_t noteCount(const daw::Project &p)
{
    size_t n = 0;
    for (const daw::Track &t : p.tracks) {
        for (const daw::Clip &c : t.clips)
            n += c.notes.size();
    }
    return n;
}

}

int runProjectSelfTest()
{
    std::printf("Project self-test: TOML and .vbp files\n\n");
    Checker c;
    std::string error;

    // The demo project through text and back
    const daw::Project demo = daw::demoProject();
    const std::string text = daw::writeProject(demo);
    daw::Project read;
    const bool ok = daw::readProject(text, read, error);
    c.check(ok, "Demo project reads back", error);
    c.check(read.tracks.size() == demo.tracks.size() && noteCount(read) == noteCount(demo), "Tracks and notes survive",
            std::to_string(read.tracks.size()) + " tracks, " + std::to_string(noteCount(read)) + " notes");
    c.check(daw::writeProject(read) == text, "Writing it again gives the same text", std::to_string(text.size()) + " bytes");
    c.check(!read.tracks.empty() && read.tracks[0].instrument.kind == audio::DeviceKind::Sampler &&
                read.tracks[0].instrument.id == "Spanish classical guitar" && read.tracks[0].instrument == demo.tracks[0].instrument &&
                read.tracks[0].kind == daw::TrackKind::Instrument && read.tracks[3].kind == daw::TrackKind::Audio &&
                read.tracks[2].effects == demo.tracks[2].effects && !read.tracks[2].effects.empty(),
            "Kinds, instruments and devices survive");
    c.check(daw::instrumentsUsed(read).size() == 1, "Instruments are listed once",
            std::to_string(daw::instrumentsUsed(read).size()) + " listed");

    // Values that must come back exactly
    daw::Project odd;
    odd.name = "Quote \" backslash \\ tab \t newline \n end";
    odd.tempo = 97.25;
    odd.bars = 7;
    odd.beatsPerBar = 3;
    daw::Track t;
    t.name = "Synth";
    t.kind = daw::TrackKind::Instrument;
    t.instrument = audio::SamplerDevice::setting("Piano [grand] = \"big\"");
    t.gain = 0.1f;
    t.pan = -0.15f;
    t.mute = true;
    t.armed = true;
    t.clips.push_back({"Thirds", 1.0 / 3.0, 2.0 / 3.0, {{1.0 / 7.0, 0.1, 61, 0.8f}, {0.0, 0.03, 0, 1.0f, true}}});
    t.clips[0].controls = {{1, {{0.0, 0.25f}, {1.0 / 3.0, 1.0f}}}, {daw::kPitchBend, {{0.1, 0.75f}}}};
    t.clips.push_back({"Empty", 4.0, 4.0, {}});
    odd.tracks.push_back(t);
    odd.loopStart = 1.0 / 3.0;
    odd.loopEnd = 5.0;
    odd.looping = true;
    daw::Project back;
    error.clear();
    const bool oddOk = daw::readProject(daw::writeProject(odd), back, error);
    bool exact = oddOk && back.name == odd.name && back.tempo == odd.tempo && back.bars == 7 && back.beatsPerBar == 3 &&
                 back.tracks.size() == 1;
    if (exact) {
        const daw::Track &b = back.tracks[0];
        exact = b.instrument == t.instrument && b.instrument.name == t.instrument.name && b.gain == t.gain && b.pan == t.pan && b.mute && b.armed && !b.solo &&
                b.clips.size() == 2 && b.clips[0].start == 1.0 / 3.0 && b.clips[0].length == 2.0 / 3.0 &&
                b.clips[0].notes.size() == 2 && b.clips[0].notes[0].start == 1.0 / 7.0 &&
                b.clips[0].notes[0].velocity == 0.8f && b.clips[0].notes[1].length == 0.03 &&
                !b.clips[0].notes[0].muted && b.clips[0].notes[1].muted && b.clips[1].notes.empty() && b.clips[0].controls.size() == 2 && b.clips[0].controls[0].controller == 1 &&
                b.clips[0].controls[0].points.size() == 2 && b.clips[0].controls[0].points[1].beat == 1.0 / 3.0 &&
                b.clips[0].controls[1].controller == daw::kPitchBend && b.clips[0].controls[1].points[0].value == 0.75f &&
                b.clips[1].controls.empty() && back.loopStart == 1.0 / 3.0 && back.loopEnd == 5.0 && back.looping;
    }
    c.check(exact, "Strings and numbers read back exactly", error);

    // Audio tracks: inputs and clips, with sound files beside the project written relative to it
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "vibin-audio-selftest";
        daw::Project a;
        daw::Track audioTrack;
        audioTrack.name = "Guitar DI";
        audioTrack.kind = daw::TrackKind::Audio;
        audioTrack.inputChannel = 1;
        audioTrack.inputStereo = true;
        audioTrack.monitor = true;
        daw::Clip take("Take 1", 8.0, 6.5);
        take.audioFile = (folder / "Audio" / "take 1.wav").string();
        take.offset = 0.375;
        take.gain = 0.5f;
        take.fadeIn = 0.01;
        take.fadeOut = 0.25;
        audioTrack.clips.push_back(take);
        daw::Clip elsewhere("Loop", 0.0, 4.0);
        elsewhere.audioFile = "C:/Samples/loop.wav";
        audioTrack.clips.push_back(elsewhere);
        a.tracks.push_back(audioTrack);
        const std::string written = daw::writeProject(a, folder.string());
        daw::Project b;
        error.clear();
        const bool readBack = daw::readProject(written, b, error, folder.string());
        bool same = readBack && b.tracks.size() == 1 && b.tracks[0].clips.size() == 2;
        if (same) {
            const daw::Track &bt = b.tracks[0];
            const daw::Clip &bc = bt.clips[0];
            same = bt.inputChannel == 1 && bt.inputStereo && bt.monitor && bc.offset == 0.375 && bc.gain == 0.5f &&
                   bc.fadeIn == 0.01 && bc.fadeOut == 0.25 &&
                   std::filesystem::path(bc.audioFile) == std::filesystem::path(take.audioFile).lexically_normal();
        }
        c.check(same, "Audio clips and inputs read back", error);
        c.check(written.find("audio_file = \"Audio/take 1.wav\"") != std::string::npos &&
                    written.find("C:/Samples/loop.wav") != std::string::npos,
                "Sound files in the project folder are relative");
    }

    // Splitting and trimming
    {
        daw::Project p;
        p.tempo = 120.0; // half a second per beat
        daw::Track inst;
        inst.kind = daw::TrackKind::Instrument;
        inst.clips.push_back({"Riff", 4.0, 8.0, {{0.0, 1.0, 60, 0.8f}, {3.0, 1.0, 62, 0.8f}, {5.0, 2.0, 64, 0.8f}}});
        p.tracks.push_back(inst);
        daw::Track audioTrack;
        audioTrack.kind = daw::TrackKind::Audio;
        daw::Clip clip("Take", 2.0, 8.0);
        clip.audioFile = "take.wav";
        clip.offset = 1.0;
        clip.fadeIn = clip.fadeOut = 0.1;
        audioTrack.clips.push_back(clip);
        p.tracks.push_back(audioTrack);

        p.select(0, 0);
        p.tracks[0].clips[0].controls = {{11, {{0.0, 0.0f}, {8.0, 1.0f}}}};
        const bool split = p.splitClip(8.0);
        const auto &parts = p.tracks[0].clips;
        c.check(split && parts.size() == 2 && parts[0].length == 4.0 && parts[1].start == 8.0 && parts[1].length == 4.0 &&
                    parts[0].notes.size() == 2 && parts[1].notes.size() == 1 && parts[1].notes[0].start == 1.0,
                "Splitting a MIDI clip shares out its notes");
        c.check(parts[0].controls.size() == 1 && parts[1].controls.size() == 1 && parts[0].controls[0].valueAt(4.0) == 0.5f &&
                    parts[1].controls[0].points.front().beat == 0.0 && parts[1].controls[0].valueAt(0.0) == 0.5f &&
                    parts[1].controls[0].valueAt(4.0) == 1.0f,
                "Splitting cuts the controller curves where they are");
        p.select(1, 0);
        p.splitClip(4.0);
        const auto &halves = p.tracks[1].clips;
        c.check(halves.size() == 2 && halves[1].offset == 2.0 && halves[0].fadeOut == 0.0 && halves[1].fadeIn == 0.0 &&
                    halves[0].fadeIn == 0.1 && halves[1].fadeOut == 0.1,
                "Splitting audio reads on from the cut", std::to_string(halves.size() == 2 ? halves[1].offset : -1.0));
        daw::Clip trimmed = halves[1];
        p.trimStart(trimmed, 5.0);
        c.check(trimmed.start == 5.0 && trimmed.length == 5.0 && trimmed.offset == 2.5, "Trimming the front moves the offset");
        p.trimStart(trimmed, 0.0);
        c.check(trimmed.offset == 0.0 && trimmed.start == 0.0, "It can't start before the file does",
                std::to_string(trimmed.start));
        p.deleteTrack(0);
        c.check(p.tracks.size() == 1 && p.selectedTrack == 0, "Deleting a track");
    }

    // Through a file
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "vibin-project-selftest.vbp";
    error.clear();
    daw::Project loaded;
    const bool saved = daw::saveProjectFile(file.string(), demo, error);
    const bool opened = saved && daw::loadProjectFile(file.string(), loaded, error);
    c.check(saved && opened && daw::writeProject(loaded) == text, "Saves to a file and opens it", error);
    error.clear();
    // Saving over it replaces it whole
    const bool resaved = daw::saveProjectFile(file.string(), odd, error) && daw::loadProjectFile(file.string(), loaded, error);
    c.check(resaved && loaded.name == odd.name && !std::filesystem::exists(file.string() + ".saving"),
            "Saving again replaces the file", error);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    error.clear();
    c.check(!daw::loadProjectFile(file.string(), loaded, error) && !error.empty(), "A missing file is an error", error);

    // TOML written by hand, in the other forms TOML allows
    const std::string hand = "\xEF\xBB\xBF# By hand\r\n"
                             "format = 3\r\n"
                             "name = 'Literal \\ string'  # a comment\n"
                             "tempo = 1_2_0\n"
                             "bars = 0x10\n"
                             "\n"
                             "[[tracks]]\n"
                             "name = \"Keys \\u00e9\"\n"
                             "kind = \"instrument\"\n"
                             "mix.ignored = true\n"
                             "[[tracks.clips]]\n"
                             "start = 4\n"
                             "notes = [\n"
                             "  {start = 0, key = 200, velocity = 3}, # clamped\n"
                             "  {start = 0.5e0, length = 2.5},\n"
                             "]\n"
                             "[[tracks.clips]]\n"
                             "name = \"Long form\"\n"
                             "[[tracks.clips.notes]]\n"
                             "key = 64\n"
                             "[[tracks.clips.notes]]\n"
                             "key = 67\n"
                             "[[tracks]]\n"
                             "name = \"Loud\"\n"
                             "gain = 9.0\n"
                             "future_feature = { anything = [1, 2, 3] }\n";
    daw::Project h;
    error.clear();
    const bool handOk = daw::readProject(hand, h, error);
    bool handRight = handOk && h.name == "Literal \\ string" && h.tempo == 120.0 && h.bars == 16 && h.tracks.size() == 2;
    if (handRight) {
        const daw::Track &k = h.tracks[0];
        handRight = k.name == "Keys \xC3\xA9" && k.clips.size() == 2 && k.clips[0].start == 4.0 &&
                    k.clips[0].notes.size() == 2 && k.clips[0].notes[0].key == 127 &&
                    k.clips[0].notes[0].velocity == 1.0f && k.clips[0].notes[1].length == 2.5 &&
                    k.clips[1].notes.size() == 2 && k.clips[1].notes[1].key == 67 &&
                    h.tracks[1].kind == daw::TrackKind::Audio && h.tracks[1].gain == 2.0f;
    }
    c.check(handRight, "Hand-written TOML reads, values clamped", error);

    // What must be refused, with a reason
    const std::pair<const char *, std::string> refused[] = {
        {"A newer format is refused", "format = 4\n"},
        {"An older format is refused", "format = 2\n"},
        {"An unknown track kind is refused", "[[tracks]]\nkind = \"video\"\n"},
        {"A value of the wrong type is refused", "[[tracks]]\ngain = \"loud\"\n"},
        {"An unterminated string is refused", "name = \"Oops\n"},
        {"A key set twice is refused", "name = \"a\"\nname = \"b\"\n"},
        {"Junk after a value is refused", "tempo = 120 bpm\n"},
        {"A date is refused", "saved = 2026-10-16\n"},
        {"Notes that aren't tables are refused", "[[tracks]]\n[[tracks.clips]]\nnotes = [60, 62]\n"},
        {"A leading zero is refused", "bars = 012\n"},
    };
    for (const auto &[what, input] : refused) {
        const std::string why = refusal(input);
        c.check(!why.empty(), what, why);
    }
    const std::string where = refusal("format = 3\n\nname = \"fine\"\ntempo = = 3\n");
    c.check(where.rfind("line 4:", 0) == 0, "Errors give the line", where);

    // The look: theme.toml round trip, defaults for missing keys, refused colours
    {
        ui::Theme t = daw::studioTheme(true, "Ember");
        t.setAccentColor(ui::Color::hex(0x4FC3B8)).setDepth(5.5f).setCornerRadius(18.0f).setRim(0.3f);
        t.setLightAngle(200.0f).setFontScale(1.1f).setControlScale(1.15f).setSpacingScale(1.4f);
        const std::string text = daw::writeTheme(t, "Ember");
        ui::Theme back = daw::studioTheme(false);
        std::string preset, why;
        const bool ok = daw::readTheme(text, back, preset, why);
        c.check(ok && preset == "Ember" && back.isDark() && back.baseColor().toHex() == t.baseColor().toHex() &&
                    back.accentColor().toHex() == t.accentColor().toHex() && back.depth() == t.depth() &&
                    back.cornerRadius() == t.cornerRadius() && back.rim() == t.rim() && back.lightAngle() == t.lightAngle() &&
                    back.fontScale() == t.fontScale() && back.controlScale() == t.controlScale() &&
                    back.spacingScale() == t.spacingScale(),
                "Theme round trip", why);
        ui::Theme partial = daw::studioTheme(false);
        why.clear();
        c.check(daw::readTheme("preset = \"Slate\"\ndark = false\ndepth = 99\n", partial, preset, why) && !partial.isDark() &&
                    partial.depth() == 10.0f && partial.rim() == daw::studioTheme().rim(),
                "Theme: missing keys keep the studio look, values clamped", why);
        ui::Theme bad = daw::studioTheme();
        why.clear();
        c.check(!daw::readTheme("accent = \"blue\"\n", bad, preset, why) && !why.empty(), "Theme: unreadable colours are refused", why);
    }

    // Undo history
    {
        daw::Project p = daw::demoProject();
        daw::History h;
        h.reset(p);
        c.check(!h.canUndo() && !h.update(p, true), "History: nothing to undo at first");
        p.select(0, 0);
        p.tracks[0].clips[0].notes[0].selected = true;
        p.tracks[0].armed = !p.tracks[0].armed;
        c.check(!h.update(p, true) && !h.canUndo(), "History: selecting and arming are not steps");
        const size_t notes = p.tracks[0].clips[0].notes.size();
        p.tracks[0].clips[0].notes.erase(p.tracks[0].clips[0].notes.begin());
        const bool waited = !h.update(p, false) && !h.canUndo();
        c.check(waited && h.update(p, true) && h.undoLabel() == "Delete note", "History: a change waits until it settles",
                h.undoLabel());
        const float gain = p.tracks[1].gain;
        p.tracks[1].gain = 0.5f;
        h.update(p, true, true, 1.0);
        p.tracks[1].gain = 0.4f;
        h.update(p, true, true, 1.2);
        const size_t merged = h.undoCount();
        p.tracks[1].gain = 0.3f;
        h.update(p, true, false, 1.3);
        c.check(merged == 2 && h.undoCount() == 3 && h.undoLabel() == "Track volume",
                "History: quick changes of one kind merge, unless asked not to", std::to_string(h.undoCount()) + " steps");
        const daw::Project *back = h.undo(p);
        c.check(back && back->tracks[1].gain == 0.4f && h.redoLabel() == "Track volume", "History: undo goes back a step");
        p = *back;
        back = h.undo(p);
        p = *back;
        back = h.undo(p);
        c.check(back && back->tracks[0].clips[0].notes.size() == notes && back->tracks[1].gain == gain &&
                    back->tracks[0].clips[0].notes[0].selected && !h.canUndo(),
                "History: undo back to the start, with the selection", std::to_string(h.redoCount()) + " to redo");
        p = *back;
        back = h.redo(p);
        c.check(back && back->tracks[0].clips[0].notes.size() == notes - 1 && h.undoLabel() == "Delete note",
                "History: redo does it again");
        p = *back;
        p.tracks[2].mute = !p.tracks[2].mute;
        c.check(h.update(p, true) && !h.canRedo() && h.undoLabel().find("ute track") != std::string::npos,
                "History: a new edit drops what could be redone", h.undoLabel());
        // An edit not yet settled is what undo takes back
        p.tracks[0].clips[0].start += 1.0;
        back = h.undo(p);
        c.check(back && back->tracks[0].clips[0].start == p.tracks[0].clips[0].start - 1.0 && h.redoLabel() == "Move clip",
                "History: undo takes back an edit still under way", h.redoLabel());
        p = *back;
        for (int i = 0; i < 250; ++i) {
            p.tempo = 60.0 + i;
            h.update(p, true);
        }
        c.check(h.undoCount() == daw::History::kMaxSteps, "History: keeps at most 200 steps",
                std::to_string(h.undoCount()));
        // 200 tempo changes share one copy of every clip, and still undo exactly
        size_t clipCount = 0;
        for (const daw::Track &track : p.tracks)
            clipCount += track.clips.size();
        const size_t stored = h.storedClips();
        const daw::Project *older = h.undo(p);
        const bool exact = older && older->tempo == 60.0 + 248 && older->tracks.size() == p.tracks.size() &&
                           daw::sameContent(*older, [&] {
                               daw::Project q = p;
                               q.tempo = 60.0 + 248;
                               return q;
                           }()) &&
                           daw::sameSelection(*older, p);
        c.check(stored == clipCount && exact, "History: steps share the clips that didn't change",
                std::to_string(stored) + " clips stored for " + std::to_string(clipCount));
        p = *older;
        const daw::Project *newer = h.redo(p);
        c.check(newer && newer->tempo == 60.0 + 249 && daw::sameSelection(*newer, p), "History: redo after sharing");
        p = *newer;
        daw::Project a = daw::demoProject();
        daw::Project b = a;
        b.tracks[1].clips.push_back(b.tracks[0].clips[0]);
        b.tracks[0].clips.erase(b.tracks[0].clips.begin());
        daw::Project t = a;
        t.addTrack(daw::TrackKind::Audio);
        daw::Project v = a;
        v.tracks[0].clips[0].notes[1].velocity = 0.2f;
        daw::Project m = a;
        m.tracks[0].clips[0].notes[2].muted = true;
        c.check(daw::describeChange(a, b) == "Move clip to another track" && daw::describeChange(a, t) == "Add track" &&
                    daw::describeChange(t, a) == "Delete track" && daw::describeChange(a, v) == "Change velocity" &&
                    daw::describeChange(a, m) == "Mute notes" && !daw::sameContent(a, m),
                "History: changes are named", daw::describeChange(a, m));
    }

    // Devices: effects saved and read back, refused when unknown, clamped, and named in the history
    {
        daw::Project p = daw::demoProject();
        audio::DeviceSetting comp = audio::effectSetting(audio::EffectType::Compressor);
        comp.setValue(0, -24.5);
        comp.bypass = true;
        p.tracks[0].effects = {audio::effectSetting(audio::EffectType::Equalizer), comp};
        p.masterEffects = {audio::effectSetting(audio::EffectType::Limiter)};
        p.masterEffects[0].setValue(1, -1.5);
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool ok = daw::readProject(text, back, error);
        c.check(ok && back.tracks[0].effects == p.tracks[0].effects && back.masterEffects == p.masterEffects &&
                    back.tracks[0].effects[1].name == "Compressor" && daw::writeProject(back) == text,
                "Devices: effects saved and read back exactly", error);
        const std::string unknown = R"(format = 3
[[tracks]]
name = "A"
[[tracks.effects]]
kind = "effect"
id = "wobble"
)";
        c.check(refusal(unknown).find("unknown effect") != std::string::npos, "Devices: an unknown effect is refused", refusal(unknown));
        const std::string unknownKind = R"(format = 3
[[master_effects]]
kind = "hologram"
id = "x"
)";
        c.check(refusal(unknownKind).find("unknown kind") != std::string::npos, "Devices: an unknown kind is refused", refusal(unknownKind));
        const std::string wild = R"(format = 3
[[tracks]]
name = "A"
[[tracks.effects]]
kind = "effect"
id = "gain"
params = [{ id = 0, value = 500.0 }, { id = 9, value = 1.0 }]
)";
        daw::Project clamped;
        daw::readProject(wild, clamped, error);
        c.check(!clamped.tracks.empty() && clamped.tracks[0].effects.size() == 1 && clamped.tracks[0].effects[0].value(0, 0) == 24.0 &&
                    clamped.tracks[0].effects[0].values.size() == 2 && clamped.tracks[0].effects[0].name == "Gain",
                "Devices: values clamped, unknown ones dropped, missing ones at their defaults", "");
        daw::Project a = daw::demoProject(), b = a, m = a;
        b.tracks[0].effects.push_back(audio::effectSetting(audio::EffectType::Reverb));
        m.masterEffects = {audio::effectSetting(audio::EffectType::Gain)};
        daw::Project t = b;
        t.tracks[0].effects[0].setValue(3, 40.0);
        c.check(daw::describeChange(a, b) == "Add effect (track)" && daw::describeChange(a, m) == "Add effect (master)" &&
                    daw::describeChange(b, t) == "Reverb settings" && !daw::sameContent(a, b),
                "Devices: changes are named", daw::describeChange(b, t));
    }

    // Plugin devices: saved and read back (state as base64, every byte), named in the history,
    // the state not counting as a change, and broken ones refused
    {
        auto pluginDevice = [](const std::string &id, const std::string &name) {
            audio::DeviceSetting d;
            d.kind = audio::DeviceKind::Plugin;
            d.id = id;
            d.name = name;
            return d;
        };
        daw::Project p = daw::demoProject();
        audio::DeviceSetting fx = pluginDevice("com.example.delay", "Example \"Delay\"");
        std::string bytes;
        for (int i = 0; i < 256; ++i)
            bytes += static_cast<char>(i);
        fx.state = std::make_shared<const std::string>(bytes + "end");
        fx.setValue(7, -6.5);
        fx.setValue(2, 0.25);
        fx.bypass = true;
        p.tracks[1].effects = {fx};
        p.masterEffects = {pluginDevice("com.example.empty", "")};
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool ok = daw::readProject(text, back, error);
        const audio::DeviceSetting *got = ok && back.tracks.size() > 1 && back.tracks[1].effects.size() == 1 ? &back.tracks[1].effects[0] : nullptr;
        c.check(got && *got == fx && got->name == fx.name && got->state && *got->state == *fx.state && got->values.size() == 2 &&
                    got->values[0].first == 2 && back.masterEffects.size() == 1 && !back.masterEffects[0].state &&
                    back.masterEffects[0].name == "com.example.empty" && back.tracks[1].effects == p.tracks[1].effects,
                "Plugin devices: saved and read back exactly (a missing name is the id)", error);
        daw::Project again;
        c.check(daw::readProject(daw::writeProject(back), again, error) && daw::writeProject(again) == daw::writeProject(back),
                "Plugin devices: a second round trip writes the same text", error);
        daw::Project changed = p;
        changed.tracks[1].effects[0].state = std::make_shared<const std::string>("other");
        c.check(daw::sameContent(p, changed), "Plugin devices: a new state alone isn't a change");
        changed.tracks[1].effects[0].setValue(7, -3.0);
        c.check(!daw::sameContent(p, changed) && daw::describeChange(p, changed) == "Example \"Delay\" settings",
                "Plugin devices: a value change is named", daw::describeChange(p, changed));
        const std::string badState = R"(format = 3
[[master_effects]]
kind = "plugin"
id = "x"
state = "not*base64"
)";
        c.check(refusal(badState).find("base64") != std::string::npos, "Plugin devices: a broken state is refused", refusal(badState));
        const std::string noId = R"(format = 3
[[master_effects]]
kind = "plugin"
)";
        c.check(refusal(noId).find("id") != std::string::npos, "Plugin devices: one without an id is refused", refusal(noId));

        // Instruments: a sampler or a plugin, never a built-in effect; a sampler never an effect
        const std::string effectInstrument = R"(format = 3
[[tracks]]
name = "A"
kind = "instrument"
[tracks.instrument]
kind = "effect"
id = "reverb"
)";
        c.check(refusal(effectInstrument).find("can't be an instrument") != std::string::npos, "Instruments: an effect isn't one",
                refusal(effectInstrument));
        const std::string samplerEffect = R"(format = 3
[[master_effects]]
kind = "sampler"
id = "Grand Piano"
)";
        c.check(refusal(samplerEffect).find("can't be an effect") != std::string::npos, "Instruments: a sampler isn't an effect",
                refusal(samplerEffect));
        const std::string loud = R"(format = 3
[[tracks]]
name = "A"
kind = "instrument"
[tracks.instrument]
kind = "sampler"
id = "Grand Piano"
params = [{ id = 0, value = 99.0 }]
[[tracks]]
name = "B"
kind = "audio"
[tracks.instrument]
kind = "sampler"
id = "Grand Piano"
)";
        daw::Project read;
        std::string why;
        c.check(daw::readProject(loud, read, why) && read.tracks.size() == 2 && read.tracks[0].instrument.id == "Grand Piano" &&
                    read.tracks[0].instrument.name == "Grand Piano" &&
                    read.tracks[0].instrument.value(audio::SamplerDevice::kLevel, 0) == 6.0 && !read.tracks[1].hasInstrument(),
                "Instruments: a sampler's level clamped, an audio track's ignored", why);

        // A plugin instrument on a track
        daw::Project withSynth = daw::demoProject();
        withSynth.tracks[0].instrument = pluginDevice("com.example.synth", "Synth");
        withSynth.tracks[0].instrument.setValue(3, 0.75);
        withSynth.tracks[0].instrument.state = std::make_shared<const std::string>("preset\n\x01");
        const std::string synthText = daw::writeProject(withSynth);
        daw::Project synthBack;
        const bool synthOk = daw::readProject(synthText, synthBack, error);
        c.check(synthOk && synthBack.tracks[0].instrument == withSynth.tracks[0].instrument &&
                    *synthBack.tracks[0].instrument.state == *withSynth.tracks[0].instrument.state &&
                    synthBack.tracks[1].instrument.kind == audio::DeviceKind::Sampler && daw::writeProject(synthBack) == synthText &&
                    daw::instrumentsUsed(synthBack).size() == 1,
                "Plugin instruments: saved and read back exactly", error);
        daw::Project other = withSynth;
        other.tracks[0].instrument.id = "com.example.other";
        daw::Project tweaked = withSynth;
        tweaked.tracks[0].instrument.setValue(3, 0.5);
        daw::Project removed = withSynth;
        removed.tracks[0].instrument = {};
        c.check(daw::describeChange(withSynth, other) == "Change instrument" && daw::describeChange(withSynth, tweaked) == "Synth settings" &&
                    daw::describeChange(withSynth, removed) == "Remove instrument",
                "Instruments: changes are named", daw::describeChange(withSynth, tweaked));
        // Places: the instrument, an effect, the master's, and none
        daw::Project places = withSynth;
        places.tracks[0].effects = {fx};
        c.check(places.device(daw::DevicePlace::instrumentOf(0)) == &places.tracks[0].instrument &&
                    places.device(daw::DevicePlace::effect(false, 0, 0)) == &places.tracks[0].effects[0] &&
                    places.device(daw::DevicePlace::effect(true, 5, 0)) == nullptr && !places.device(daw::DevicePlace::effect(false, 0, 1)) &&
                    !places.device(daw::DevicePlace::instrumentOf(3)) && places.effects(true, -1) == &places.masterEffects,
                "Devices: found by place");
    }

    // The built-in synth: saved and read back exactly (a choice, a negative bipolar value), values
    // clamped into range, unknown ids dropped, its name as it was, and refused as an effect
    {
        daw::Project p = daw::demoProject();
        audio::DeviceSetting synth = audio::SynthDevice::setting();
        synth.name = "Vibin Synth";
        synth.setValue(0, 3.0);     // Osc 1 wave: Square
        synth.setValue(34, -40.0);  // filter envelope amount: bipolar
        synth.setValue(31, 1234.5); // cutoff
        synth.bypass = false;
        p.tracks[0].instrument = synth;
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool ok = daw::readProject(text, back, error);
        c.check(ok && back.tracks[0].instrument == synth && back.tracks[0].instrument.name == "Vibin Synth" &&
                    back.tracks[0].instrument.kind == audio::DeviceKind::Synth && daw::writeProject(back) == text &&
                    daw::instrumentsUsed(back).size() == 1 && daw::instrumentsUsed(back)[0] == "Spanish classical guitar",
                "Synth: saved and read back exactly, and not a library instrument", error);
        const std::string wild = R"(format = 3
[[tracks]]
name = "A"
kind = "instrument"
[tracks.instrument]
kind = "synth"
id = "vibin.synth"
params = [{ id = 0, value = 99.0 }, { id = 34, value = -5.0 }, { id = 9999, value = 1.0 }]
)";
        daw::Project clamped;
        const bool wildOk = daw::readProject(wild, clamped, error);
        const audio::DeviceSetting &s = clamped.tracks.empty() ? audio::DeviceSetting() : clamped.tracks[0].instrument;
        const std::vector<audio::DeviceParam> known = audio::synth::synthParams();
        double waveMax = -1.0; // the wave is a choice: 99 is clamped to its last
        for (const audio::DeviceParam &p : known)
            if (p.id == 0)
                waveMax = p.max;
        c.check(wildOk && s.kind == audio::DeviceKind::Synth && s.value(0, 0) == waveMax && waveMax == 7.0 && s.value(34, 0) == -5.0 &&
                    s.values.size() == known.size() && s.value(9999, -1) == -1 && s.name == "Init",
                "Synth: values clamped, unknown ids dropped, missing ones at their defaults", error);
        const std::string wrongId = R"(format = 3
[[tracks]]
name = "A"
kind = "instrument"
[tracks.instrument]
kind = "synth"
id = "vibin.wobble"
)";
        c.check(refusal(wrongId).find("unknown synth") != std::string::npos, "Synth: an unknown synth is refused", refusal(wrongId));
        const std::string synthEffect = R"(format = 3
[[master_effects]]
kind = "synth"
id = "vibin.synth"
)";
        c.check(refusal(synthEffect).find("can't be an effect") != std::string::npos, "Synth: it isn't an effect", refusal(synthEffect));
    }

    // Export: the range and the stems' names
    {
        daw::Project p = daw::demoProject();
        double last = 0.0;
        for (const daw::Track &t : p.tracks) {
            for (const daw::Clip &clip : t.clips)
                last = std::max(last, clip.start + clip.length);
        }
        daw::ExportTarget song;
        song.range = daw::ExportRangeKind::Song;
        double from = -1.0, to = -1.0;
        daw::exportTargetRange(p, song, from, to);
        c.check(from == 0.0 && to == std::max(last, static_cast<double>(p.beatsPerBar)), "Export: a song runs to its last clip",
                "to " + std::to_string(to) + " beats");
    }

    // The document: saved state, the file, and the history
    {
        daw::ProjectSession s(daw::demoProject());
        c.check(!s.modified() && s.path().empty(), "Session: a new session is saved and has no file");
        const double tempo = s.project().tempo;
        s.store().setTempo(tempo + 12.0);
        s.checkModified();
        c.check(s.modified(), "Session: an edit makes it modified");
        s.markSaved("C:/Mixes/Demo.vbp");
        c.check(!s.modified() && s.path() == "C:/Mixes/Demo.vbp", "Session: saving clears modified and sets the file");
        s.store().setTempo(tempo);
        s.checkModified();
        c.check(s.modified(), "Session: changing back after a save is modified again");
        s.store().setTempo(tempo + 12.0);
        s.history().update(s.project(), true);
        c.check(s.history().canUndo(), "Session: an edit is a history step");
        s.replace(daw::demoProject(), "");
        c.check(!s.modified() && s.path().empty() && !s.history().canUndo(),
                "Session: replacing the project resets saved state and history");
    }

    // The store: each edit moves the revisions it should, and nothing else
    {
        daw::ProjectStore store(daw::demoProject());
        c.check(store.revision() == 1 && store.structureRevision() == 1 && store.trackRevision(0) == 1 &&
                    store.mixRevision(0) == 1 && store.trackRevision(99) == 0,
                "Store: revisions start at 1, out of range is 0");
        const size_t tracks = store.project().tracks.size();

        uint64_t r0 = store.revision(), t0 = store.trackRevision(0), t1 = store.trackRevision(1);
        store.editClip(0, 0).name = "Store test";
        c.check(store.revision() > r0 && store.trackRevision(0) > t0 && store.trackRevision(1) == t1,
                "Store: a clip edit moves its track and not another's");

        const uint64_t m1 = store.mixRevision(1);
        r0 = store.revision();
        t1 = store.trackRevision(1);
        store.editMix(1).gain = 0.5f;
        c.check(store.mixRevision(1) > m1 && store.revision() > r0 && store.trackRevision(1) == t1,
                "Store: a mix edit moves the mix and not the track");

        store.select(1, 0);
        r0 = store.revision();
        const uint64_t s0 = store.selectionRevision();
        store.select(2, 0);
        c.check(store.selectionRevision() > s0 && store.revision() == r0, "Store: a selection moves only the selection");

        const double tempo = store.project().tempo;
        r0 = store.revision();
        store.setTempo(tempo);
        const bool tempoSame = store.revision() == r0;
        store.setTempo(tempo + 7.0);
        c.check(tempoSame && store.revision() > r0, "Store: a tempo moves the revision only when it differs");

        const daw::Project &p = store.project();
        r0 = store.revision();
        store.setLoop(p.loopStart, p.loopEnd, p.looping);
        const bool loopSame = store.revision() == r0;
        store.setLoop(1.0, 9.0, true);
        c.check(loopSame && store.revision() > r0 && store.project().loopEnd == 9.0,
                "Store: a loop moves the revision only when it differs");

        r0 = store.revision();
        store.toggleLoop();
        c.check(store.revision() > r0, "Store: toggling the loop moves the revision");

        std::vector<uint64_t> before;
        for (size_t i = 0; i < tracks; ++i)
            before.push_back(store.trackRevision(static_cast<int>(i)));
        const uint64_t structure = store.structureRevision();
        store.edit();
        bool allMoved = store.structureRevision() > structure;
        for (size_t i = 0; i < tracks; ++i)
            allMoved = allMoved && store.trackRevision(static_cast<int>(i)) > before[i];
        c.check(allMoved, "Store: edit() moves every track and the structure");

        store.edit().addTrack(daw::TrackKind::Audio);
        const int added = static_cast<int>(store.project().tracks.size()) - 1;
        c.check(store.trackRevision(added) != 0 && store.structureRevision() > structure, "Store: a new track has a revision");

        const uint64_t m = store.masterRevision();
        r0 = store.revision();
        store.editMasterEffects();
        c.check(store.masterRevision() > m && store.revision() > r0, "Store: master effects move the master revision");

        // A device at a place: its track moves and the setting comes back; a place with none moves nothing
        const uint64_t dt = store.trackRevision(0), dr = store.revision(), dm = store.masterRevision();
        const bool hasInstrument = store.project().device(daw::DevicePlace::instrumentOf(0)) != nullptr;
        auto edited = store.editDevice(daw::DevicePlace::instrumentOf(0));
        c.check(hasInstrument && edited && store.trackRevision(0) > dt && store.revision() > dr && store.masterRevision() == dm,
                "Store: editing a device moves its track and gives the setting back");
        const uint64_t dt2 = store.trackRevision(0), dr2 = store.revision();
        c.check(store.editDevice(daw::DevicePlace::effect(true, 0, 99)) == nullptr && store.trackRevision(0) == dt2 &&
                    store.revision() == dr2,
                "Store: a missing device is no edit");

        const uint64_t rev = store.revision(), master = store.masterRevision(), sel = store.selectionRevision(),
                       str = store.structureRevision();
        store.replace(daw::demoProject());
        const bool replaced = store.revision() > rev && store.masterRevision() > master && store.selectionRevision() > sel &&
                              store.structureRevision() > str && store.trackRevision(0) > 1 && store.mixRevision(0) > 1;
        c.check(replaced, "Store: replace moves everything");
        const uint64_t revAfter = store.revision(), structAfter = store.structureRevision(), trackAfter = store.trackRevision(0);
        store.setTempo(store.project().tempo);
        c.check(store.revision() >= revAfter && store.structureRevision() >= structAfter && store.trackRevision(0) >= trackAfter,
                "Store: revisions only increase");
    }

    // Idle frames: the session's revision checks against scanning the whole project every frame. The
    // timing is printed (not a check); the checks after it are the behaviour.
    {
        daw::Project big;
        for (int t = 0; t < 16; ++t) {
            const int index = big.addTrack(daw::TrackKind::Instrument);
            daw::Track &track = big.tracks[static_cast<size_t>(index)];
            track.clips.clear();
            for (int k = 0; k < 40; ++k) {
                std::vector<daw::Note> notes(250);
                for (size_t n = 0; n < notes.size(); ++n) {
                    notes[n].start = static_cast<double>(n) * 0.016;
                    notes[n].length = 0.25;
                    notes[n].key = 40 + static_cast<int>(n % 40);
                    notes[n].velocity = 0.7f;
                }
                track.clips.push_back(daw::Clip("Clip " + std::to_string(k), k * 4.0, 4.0, std::move(notes)));
            }
            if (t < 3) {
                daw::ControlLane lane;
                lane.controller = 1;
                daw::ControlPoint first, second;
                first.beat = 0.0;
                first.value = 0.2f;
                second.beat = 2.0;
                second.value = 0.8f;
                lane.points = {first, second};
                track.clips[0].controls.push_back(lane);
            }
        }
        const size_t notes = noteCount(big);
        const int frames = 300;
        using Clock = std::chrono::steady_clock;
        auto elapsed = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };

        daw::ProjectSession session(big);
        session.history().reset(session.project());
        // The first frame after a project is made compares it once (and writes its saved text), so it
        // is timed apart from the idle frames that follow
        const Clock::time_point firstStart = Clock::now();
        session.recordHistory(true, true, 1.0);
        session.checkModified();
        const double firstMs = elapsed(firstStart);
        const Clock::time_point revisionStart = Clock::now();
        for (int f = 0; f < frames; ++f) {
            session.recordHistory(true, true, 1.0 + f / 60.0);
            session.checkModified();
        }
        const double revisionMs = elapsed(revisionStart);

        // The old way: the history compares the whole project each frame, and the saved text is
        // written and compared every 15th frame
        const std::string savedText = daw::writeProject(big);
        daw::History scan;
        scan.reset(big);
        int textChecks = 0;
        const Clock::time_point scanStart = Clock::now();
        for (int f = 0; f < frames; ++f) {
            scan.update(big, true, true, 1.0 + f / 60.0);
            if (f % 15 == 0 && daw::writeProject(big) != savedText)
                ++textChecks;
        }
        const double scanMs = elapsed(scanStart);

        // An edit after the idle frames is recorded once it settles
        const size_t before = session.history().undoCount();
        session.store().editClip(0, 0).name = "Idle edit";
        const bool waited = !session.recordHistory(false, false, 2.0);
        const bool recorded = session.recordHistory(true, false, 2.1);
        c.check(waited && recorded && session.history().undoCount() == before + 1 &&
                    session.history().undoLabel() == "Rename clip",
                "Idle frames: an edit after them is recorded once settled", session.history().undoLabel());

        // One saved-state check after an edit: the new way (sameSaved against the saved copy), and the
        // old way (writing the whole project and comparing its text)
        // The name goes back, and the edit is at the very end of the project, so the compare goes
        // through everything before it
        session.store().editClip(0, 0).name = "Clip 0";
        session.store().editClip(15, 39).notes.back().velocity = 0.3f;
        const Clock::time_point checkStart = Clock::now();
        session.checkModified();
        const double checkMs = elapsed(checkStart);
        const Clock::time_point textStart = Clock::now();
        const bool textDiffers = daw::writeProject(session.project()) != savedText;
        const double textMs = elapsed(textStart);
        c.check(session.modified() && textDiffers, "Saved state: the check after an edit agrees with the text");
        std::printf("INFO idle frame: %.4f ms with revisions (first frame %.1f ms), %.3f ms scanning (%zu notes, %d text checks); "
                    "one check after an edit: %.3f ms, %.1f ms writing and comparing the text\n",
                    revisionMs / frames, firstMs, scanMs / frames, notes, textChecks, checkMs, textMs);
    }

    // A selection-only change is no step, and undo puts the selection back as it was
    {
        daw::ProjectSession s(daw::demoProject());
        s.history().reset(s.project());
        s.store().select(0, 0);
        s.store().editClip(0, 0).name = "First";
        const bool first = s.recordHistory(true, false, 1.0);
        const size_t steps = s.history().undoCount();
        s.store().select(1, 0);
        const bool selectOnly = s.recordHistory(true, false, 2.0);
        s.store().editClip(0, 0).name = "Second";
        const bool second = s.recordHistory(true, false, 3.0);
        c.check(first && !selectOnly && second && s.history().undoCount() == steps + 1,
                "Idle history: a selection-only change is no step", std::to_string(s.history().undoCount()) + " steps");
        const daw::Project *back = s.history().undo(s.project(), 4.0);
        c.check(back && back->selectedTrack == 1 && back->tracks[0].clips[0].name == "First",
                "Idle history: undo restores the selection as it was");
    }

    // Checking for unsaved changes notices an edit, and a second edit back to the saved text is not modified
    {
        daw::ProjectSession s(daw::demoProject());
        const std::string original = s.project().tracks[0].clips[0].name;
        s.checkModified();
        s.store().editClip(0, 0).name = "Changed";
        s.checkModified();
        const bool noticed = s.modified();
        s.store().editClip(0, 0).name = original;
        s.checkModified();
        c.check(noticed && !s.modified(), "Idle checks: an edit is noticed, and reverting it is not modified");
    }

    // The saved state keeps arming (a change), but not the selection (no change)
    {
        daw::ProjectSession s(daw::demoProject());
        s.store().select(1, 0);
        s.checkModified();
        const bool selectionOnly = !s.modified();
        daw::Track &mix = s.store().editMix(1);
        mix.armed = !mix.armed;
        s.checkModified();
        c.check(selectionOnly && s.modified(), "Saved state: arming a track is a change, the selection isn't");
    }

    // Export presets and details: every enum used, read back exactly, and written as the file says
    {
        daw::Project tp = daw::demoProject();
        tp.exportMetadata = {"Album \"X\" \\ tab\t", "Artist", "", "line one\nline two"};
        daw::ExportTarget a;
        a.name = "Master";
        a.source = daw::ExportSource::Master;
        a.tap = daw::ExportTap::PreFader;
        a.range = daw::ExportRangeKind::Bars;
        a.fromBar = 2.5;
        a.toBar = 6;
        a.tailSeconds = 1.25;
        a.format = daw::ExportFormat::Aiff;
        a.bits = 16;
        a.sampleRate = 44100;
        a.mono = true;
        a.normalize = daw::NormalizeMode::Peak;
        a.peakDb = -1.5;
        a.limiter = false;
        a.fadeInMs = 12.5;
        a.fadeOutMs = 800;
        a.dither = daw::DitherMode::None;
        a.fileName = "{project} {date}";
        a.folder = "Exports/Final";
        daw::ExportTarget b;
        b.name = "Drums";
        b.source = daw::ExportSource::Tracks;
        b.tracks = {"Drums", "Bass [sub] \"x\""};
        b.respectMuteSolo = false;
        b.range = daw::ExportRangeKind::Loop;
        b.enabled = false;
        b.format = daw::ExportFormat::Flac;
        b.bits = 32;
        b.normalize = daw::NormalizeMode::Loudness;
        b.loudnessLufs = -16.5;
        b.ceilingDbtp = -1.2;
        b.dither = daw::DitherMode::NoiseShaped;
        daw::ExportTarget d;
        d.name = "Dry";
        d.source = daw::ExportSource::MasterDry;
        d.range = daw::ExportRangeKind::Timeline;
        daw::ExportTarget e;
        e.name = "Stems";
        e.source = daw::ExportSource::EachTrack;
        daw::ExportTarget m;
        m.name = "MIDI";
        m.source = daw::ExportSource::Midi;
        tp.exportPresets = {{"Everything \"quoted\"", {a, b, d, e, m}}, {"Empty", {}}};

        const std::string tpText = daw::writeProject(tp);
        daw::Project tpBack;
        error.clear();
        const bool tpOk = daw::readProject(tpText, tpBack, error);
        c.check(tpOk && tpBack.exportPresets == tp.exportPresets && tpBack.exportMetadata == tp.exportMetadata &&
                    daw::writeProject(tpBack) == tpText,
                "Export: presets and details read back exactly", error);

        // No presets is written as none, not as the defaults coming back
        tp.exportPresets.clear();
        tp.exportMetadata = {};
        daw::Project none;
        error.clear();
        const bool noneOk = daw::readProject(daw::writeProject(tp), none, error);
        c.check(noneOk && none.exportPresets.empty() && none.exportMetadata == daw::ExportMetadata{},
                "Export: no presets stays none", error);

        // A file with no presets key gets the defaults
        daw::Project bare;
        error.clear();
        const bool bareOk = daw::readProject("format = 3\nname = \"Bare\"\n", bare, error);
        c.check(bareOk && bare.exportPresets == daw::defaultExportPresets() && bare.exportMetadata == daw::ExportMetadata{},
                "Export: a file without presets gets the defaults", error);

        // Unknown names are refused with a reason; values out of range are clamped and unknown keys ignored
        const std::string badSource = "[[export_presets]]\nname = \"Bad\"\n\n[[export_presets.targets]]\nsource = \"everything\"\n";
        c.check(refusal(badSource).find("unknown export source") != std::string::npos, "Export: an unknown source is refused",
                refusal(badSource));
        const std::string badTap = "[[export_presets]]\nname = \"Bad\"\n\n[[export_presets.targets]]\ntap = \"sideways\"\n";
        c.check(refusal(badTap).find("unknown export tap") != std::string::npos, "Export: an unknown tap is refused", refusal(badTap));

        const std::string wild = "[[export_presets]]\nname = \"Wild\"\n\n[[export_presets.targets]]\nbits = 12\nsample_rate = 12345\n"
                                 "tail_seconds = 99\nloudness_lufs = -60\npeak_db = 3\nceiling_dbtp = -30\nfade_in_ms = 90000\n"
                                 "fade_out_ms = -5\nfrom_bar = 0\nto_bar = 0\nsurprise = 1\n";
        daw::Project wildP;
        std::string wildWhy;
        const bool wildOk = daw::readProject(wild, wildP, wildWhy);
        const daw::ExportTarget *w = wildOk && !wildP.exportPresets.empty() && !wildP.exportPresets[0].targets.empty()
                                         ? &wildP.exportPresets[0].targets[0]
                                         : nullptr;
        const bool clamped = w && w->bits == 24 && w->sampleRate == 48000 && w->tailSeconds == 30.0 &&
                             w->loudnessLufs == -40.0 && w->peakDb == 0.0 && w->ceilingDbtp == -20.0 &&
                             w->fadeInMs == 60000.0 && w->fadeOutMs == 0.0 && w->fromBar == 1.0 && w->toBar == 2.0;
        c.check(clamped, "Export: out-of-range values are clamped, unknown keys ignored", wildWhy);
    }

    // File names: each token, unsafe characters, EachTrack outputs and the extensions
    {
        std::tm local{};
        local.tm_year = 2026 - 1900;
        local.tm_mon = 2;
        local.tm_mday = 5;
        local.tm_hour = 14;
        local.tm_min = 7;
        local.tm_isdst = -1;
        const std::time_t now = std::mktime(&local);
        daw::Project np;
        np.name = "Night Mix";
        daw::ExportTarget mix;
        const std::string n1 = daw::resolveFileName(mix, np, "Mix", "", now);
        c.check(n1 == "Night Mix.wav", "File names: the project's name and the extension", n1);

        daw::ExportTarget t = mix;
        t.name = "Mix";
        t.fileName = "{project} {target} {date} {time}";
        const std::string n2 = daw::resolveFileName(t, np, "Mix", "", now);
        c.check(n2 == "Night Mix Mix 2026-03-05 14-07.wav", "File names: target, date and time", n2);

        t.fileName = "{rate}_{bits}bit_{format}";
        t.sampleRate = 44100;
        const std::string n3 = daw::resolveFileName(t, np, "Mix", "", now);
        t.sampleRate = 48000;
        const std::string n4 = daw::resolveFileName(t, np, "Mix", "", now);
        c.check(n3 == "44.1k_24bit_wav.wav" && n4 == "48k_24bit_wav.wav", "File names: rate, bits and format", n3 + " / " + n4);

        t = mix;
        t.format = daw::ExportFormat::Flac;
        t.fileName = "{preset}";
        const std::string n5 = daw::resolveFileName(t, np, "Release", "", now);
        c.check(n5 == "Release.flac", "File names: the preset and FLAC", n5);

        np.name = "A/B:C*?";
        const std::string n6 = daw::resolveFileName(mix, np, "Mix", "", now);
        c.check(n6 == "A_B_C__.wav", "File names: characters Windows refuses become _", n6);
        np.name = "Night Mix";

        t = mix;
        t.source = daw::ExportSource::EachTrack;
        const std::string n7 = daw::resolveFileName(t, np, "Stems", "Bass", now);
        const std::string n8 = daw::resolveFileName(t, np, "Stems", "Lead|2", now);
        c.check(n7 == "Night Mix - Bass.wav" && n8 == "Night Mix - Lead_2.wav", "File names: EachTrack adds the track", n7 + " / " + n8);
        t.fileName = "{project} {track}";
        const std::string n9 = daw::resolveFileName(t, np, "Stems", "Bass", now);
        c.check(n9 == "Night Mix Bass.wav", "File names: a {track} token is used instead", n9);
        t.fileName = "{project}";
        const std::string n10 = daw::resolveFileName(t, np, "Stems", "", now);
        c.check(n10 == "Night Mix.wav", "File names: no track name, no suffix", n10);

        t = mix;
        t.source = daw::ExportSource::Midi;
        const std::string n11 = daw::resolveFileName(t, np, "MIDI", "", now);
        t.fileName = "{project}.mid";
        const std::string n12 = daw::resolveFileName(t, np, "MIDI", "", now);
        c.check(n11 == "Night Mix.mid" && n12 == "Night Mix.mid", "File names: MIDI is .mid, the extension isn't doubled",
                n11 + " / " + n12);
        t = mix;
        t.fileName = "{project}.WAV";
        const std::string n13 = daw::resolveFileName(t, np, "Mix", "", now);
        t.format = daw::ExportFormat::Aiff;
        t.fileName = "{project}";
        const std::string n14 = daw::resolveFileName(t, np, "Mix", "", now);
        c.check(n13 == "Night Mix.WAV" && n14 == "Night Mix.aiff", "File names: the extension is case-blind and AIFF is .aiff",
                n13 + " / " + n14);
    }

    // The part of the song each range covers
    {
        daw::Project rp;
        rp.bars = 16;
        rp.beatsPerBar = 4;
        rp.tracks.push_back(daw::Track{});
        rp.tracks[0].clips.push_back(daw::Clip("A", 8.0, 4.0));
        rp.tracks[0].clips.push_back(daw::Clip("B", 20.0, 2.0));
        double s = -1.0, e = -1.0;
        daw::ExportTarget target;
        const bool songOk = daw::exportTargetRange(rp, target, s, e) && s == 0.0 && e == 22.0;
        target.range = daw::ExportRangeKind::Timeline;
        const bool timelineOk = daw::exportTargetRange(rp, target, s, e) && s == 0.0 && e == 64.0;
        target.range = daw::ExportRangeKind::Loop;
        const bool noLoop = !daw::exportTargetRange(rp, target, s, e);
        rp.loopStart = 4.0;
        rp.loopEnd = 12.0;
        const bool loopOk = daw::exportTargetRange(rp, target, s, e) && s == 4.0 && e == 12.0;
        target.range = daw::ExportRangeKind::Bars;
        target.fromBar = 2;
        target.toBar = 4;
        const bool barsOk = daw::exportTargetRange(rp, target, s, e) && s == 4.0 && e == 12.0;
        target.fromBar = 3;
        target.toBar = 3;
        const bool emptyBars = !daw::exportTargetRange(rp, target, s, e);
        daw::Project none;
        none.bars = 16;
        none.beatsPerBar = 4;
        target.range = daw::ExportRangeKind::Song;
        const bool emptySong = daw::exportTargetRange(none, target, s, e) && s == 0.0 && e == 4.0;
        c.check(songOk && timelineOk && noLoop && loopOk && barsOk && emptyBars && emptySong, "Export: ranges for song, timeline, loop and bars",
                std::to_string(s) + " to " + std::to_string(e));
    }

    // Export presets and details are saved edits, but no track's revision moves
    {
        daw::ProjectStore store(daw::demoProject());
        const uint64_t r0 = store.revision(), t0 = store.trackRevision(0), m0 = store.mixRevision(0), s0 = store.structureRevision();
        store.editExportPresets().push_back(daw::ExportPreset{"Extra", {}});
        const bool presetMoved = store.revision() > r0 && store.trackRevision(0) == t0 && store.mixRevision(0) == m0 &&
                                 store.structureRevision() == s0 && store.project().exportPresets.back().name == "Extra";
        const uint64_t r1 = store.revision();
        store.editExportMetadata().title = "Title";
        const bool metaMoved = store.revision() > r1 && store.trackRevision(0) == t0 && store.project().exportMetadata.title == "Title";
        c.check(presetMoved && metaMoved, "Store: export presets and details move the revision, not a track's");
    }

    // The history records a change to a preset or the details, and undo takes it back
    {
        daw::Project hp = daw::demoProject();
        daw::History h;
        h.reset(hp);
        hp.exportPresets[0].targets[0].bits = 16;
        const bool preset = h.update(hp, true, false, 1.0);
        const std::string presetLabel = h.undoLabel();
        hp.exportMetadata.title = "Title";
        const bool details = h.update(hp, true, false, 2.0);
        c.check(preset && presetLabel == "Edit export preset" && details && h.undoLabel() == "Edit export details",
                "History: export presets and details are steps", presetLabel + ", " + h.undoLabel());
        const daw::Project *back = h.undo(hp, 3.0);
        c.check(back && back->exportMetadata.title.empty() && back->exportPresets[0].targets[0].bits == 16,
                "History: undo takes the details back, keeping the preset");
    }

    // Autosave: keys, manifests, rotation, the session lock and recovery, and real writes through the
    // controller into a temporary folder (never the user's autosave folder)
    {
        namespace fs = std::filesystem;
        const fs::path base = fs::temp_directory_path() / "vibin-autosave-selftest";
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(base, ec);
        const fs::path root = base / "root";
        // Reads a file and lets it go at once (an open file can't be replaced on Windows)
        auto slurp = [](const fs::path &p) {
            std::ifstream in(p, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        };
        auto version =[](const std::string &file, double time, const std::string &kind, const std::string &label) {
            daw::AutosaveVersion v;
            v.file = file;
            v.time = time;
            v.kind = kind;
            v.label = label;
            return v;
        };

        const std::string keyA = daw::autosaveKey("C:\\Music\\Night.vbp");
        c.check(keyA == daw::autosaveKey("c:/music/night.vbp") && keyA.size() == 16 &&
                    keyA != daw::autosaveKey("c:/music/day.vbp"),
                "Autosave: keys are stable across spellings and distinct", keyA);
        c.check(daw::untitledKey(100.0, 7) != daw::untitledKey(100.0, 8) &&
                    daw::untitledKey(100.0, 7).rfind("untitled-", 0) == 0,
                "Autosave: unsaved sessions get their own keys");
        const std::string name = daw::versionFileName(1700000000.0);
        c.check(name.size() == 28 && name.rfind("autosave-", 0) == 0 && name.substr(name.size() - 4) == ".vbp" &&
                    daw::versionFileName(1700000001.0) != name,
                "Autosave: version file names carry the time", name);

        daw::AutosaveManifest manifest;
        manifest.path = "C:\\Music\\\"Night\".vbp";
        manifest.name = "Night \"Mix\"";
        manifest.updated = 1700000100.5;
        manifest.versions = {version("autosave-a.vbp", 1700000000.0, "auto", ""),
                             version("autosave-b.vbp", 1700000050.25, "manual", "Before mix\n2")};
        daw::AutosaveManifest back;
        std::string manifestError;
        const bool parsed = daw::parseManifest(daw::writeManifest(manifest), back, manifestError);
        c.check(parsed && back.path == manifest.path && back.name == manifest.name && back.updated == manifest.updated &&
                    back.versions.size() == 2 && back.versions[0].time == 1700000000.0 &&
                    back.versions[1].kind == "manual" && back.versions[1].label == "Before mix\n2",
                "Autosave: a manifest reads back as written", manifestError);

        std::vector<daw::AutosaveVersion> list;
        for (int i = 1; i <= 5; ++i)
            list.push_back(version("auto" + std::to_string(i) + ".vbp", i * 10.0, "auto", ""));
        list.push_back(version("manual-a.vbp", 5.0, "manual", "Keep"));
        list.push_back(version("manual-b.vbp", 1.0, "manual", ""));
        const std::vector<daw::AutosaveVersion> removed = daw::rotate(list, 3);
        const bool oldestGone = removed.size() == 2 && removed[0].time == 10.0 && removed[1].time == 20.0;
        size_t manuals = 0;
        double oldestAuto = 1e18;
        for (const daw::AutosaveVersion &v : list) {
            if (v.kind == "manual")
                ++manuals;
            else
                oldestAuto = std::min(oldestAuto, v.time);
        }
        c.check(oldestGone && list.size() == 5 && manuals == 2 && oldestAuto == 30.0,
                "Autosave: rotation keeps the newest automatic versions and every manual one");

        daw::SessionLock lock;
        lock.pid = 4321;
        lock.started = 1700000000.5;
        lock.key = keyA;
        lock.path = "C:\\Music\\Night.vbp";
        daw::SessionLock lockBack;
        std::string lockError;
        const bool lockParsed = daw::parseSessionLock(daw::writeSessionLock(lock), lockBack, lockError);
        c.check(lockParsed && lockBack.pid == 4321 && lockBack.started == lock.started && lockBack.key == keyA &&
                    lockBack.path == lock.path,
                "Autosave: a session lock reads back as written", lockError);

        const double now = 1700000200.0;
        c.check(!daw::isRecoverable(lock, now, true, 1700000100.0, 0.0) &&
                    daw::isRecoverable(lock, now, false, 1700000100.0, 1700000050.0) &&
                    !daw::isRecoverable(lock, now, false, 1700000040.0, 1700000050.0) &&
                    daw::isRecoverable(lock, now, false, 1700000100.0, 0.0) &&
                    !daw::isRecoverable(lock, now, false, 0.0, 0.0),
                "Autosave: recoverable when the process is gone and an autosave is newer than the file");

        // The controller, with a fake clock and no environment (its writes run on this thread)
        double clock = 1700000000.0;
        auto fakeNow = [&clock] { return clock; };
        daw::ProjectSession session(daw::demoProject());
        const std::string key = [&] {
            daw::AutosaveController probe(nullptr, root.string(), fakeNow);
            return probe.keyFor(session);
        }();
        {
            daw::AutosaveController ctl(nullptr, root.string(), fakeNow);
            ctl.configure(true, 120.0, 3);
            std::string error;
            ctl.update(session, true, error);
            daw::SessionLock first;
            const bool locked = daw::readSessionLockFile(root.string(), first);
            c.check(locked && first.pid > 0 && first.key == ctl.keyFor(session) && first.path.empty(),
                    "Autosave: the session lock is written at the start", error);

            // An unmodified project is never autosaved
            clock += 500.0;
            ctl.update(session, true, error);
            const bool quiet = !fs::exists(root / key);
            c.check(quiet, "Autosave: nothing is written while the project is unmodified");

            // A change: waits for the interval, and not for unsettled frames
            session.store().edit().name = "Night";
            session.checkModified();
            ctl.update(session, true, error);
            clock += 130.0;
            const bool unsettled = ctl.update(session, false, error) == daw::AutosaveController::Event::None &&
                                   !fs::exists(root / key / "manifest.toml");
            c.check(unsettled, "Autosave: an unsettled edit is not written");
            ctl.update(session, true, error);
            const bool wrote = ctl.update(session, true, error) == daw::AutosaveController::Event::Autosaved;
            c.check(wrote && fs::exists(root / key / "manifest.toml"), "Autosave: a settled change is written after the interval",
                    error);
            // The newest file reads back as the project
            daw::AutosaveManifest listed;
            std::string parseError;
            bool exists = false;
            const std::string manifestText = slurp(root / key / "manifest.toml");
            exists = daw::parseManifest(manifestText, listed, parseError);
            daw::Project read;
            std::string readError;
            const bool readBack = exists && !listed.versions.empty() &&
                                  daw::loadProjectFile((root / key / listed.versions.back().file).string(), read, readError);
            c.check(readBack && daw::writeProject(read) == daw::writeProject(session.project()) && read.name == "Night",
                    "Autosave: the version reads back as the project", readError);

            // Rotation: four more changes keep three automatic versions on disk
            for (int i = 2; i <= 5; ++i) {
                session.store().edit().name = "Night " + std::to_string(i);
                session.checkModified();
                ctl.update(session, true, error);
                clock += 130.0;
                ctl.update(session, true, error);
                ctl.update(session, true, error);
            }
            size_t files = 0;
            for (const auto &entry : fs::directory_iterator(root / key)) {
                if (entry.path().extension() == ".vbp")
                    ++files;
            }
            const std::string againText = slurp(root / key / "manifest.toml");
            daw::AutosaveManifest after;
            daw::parseManifest(againText, after, parseError);
            c.check(files == 3 && after.versions.size() == 3, "Autosave: rotation removes the oldest files beyond the count",
                    std::to_string(files) + " files");

            // A manual version is written at once and never rotated
            ctl.saveVersion(session, "Before mix");
            const bool manualSaved = ctl.update(session, true, error) == daw::AutosaveController::Event::VersionSaved;
            for (int i = 6; i <= 7; ++i) {
                session.store().edit().name = "Night " + std::to_string(i);
                session.checkModified();
                ctl.update(session, true, error);
                clock += 130.0;
                ctl.update(session, true, error);
                ctl.update(session, true, error);
            }
            const std::string lastText = slurp(root / key / "manifest.toml");
            daw::AutosaveManifest kept;
            daw::parseManifest(lastText, kept, parseError);
            size_t manualKept = 0;
            for (const daw::AutosaveVersion &v : kept.versions)
                manualKept += v.kind == "manual" && v.label == "Before mix" ? 1 : 0;
            c.check(manualSaved && manualKept == 1 && kept.versions.size() == 4,
                    "Autosave: a manual version is kept while automatic ones rotate", parseError);

            // Saving the project to a file moves the lock and the folder to its path
            session.markSaved((base / "Night.vbp").string());
            ctl.update(session, true, error);
            daw::SessionLock moved;
            const bool movedLock = daw::readSessionLockFile(root.string(), moved);
            c.check(movedLock && moved.path == (base / "Night.vbp").string() && moved.key != key &&
                        moved.key == daw::autosaveKey((base / "Night.vbp").string()),
                    "Autosave: the lock follows the project's file after a save");
        }
        // A clean exit (the controller's destruction) removes the lock
        c.check(!fs::exists(root / "session.lock"), "Autosave: a clean exit removes the session lock");

        // A crash: a lock left behind by a process that isn't running, with an autosave newer than the file
        fs::create_directories(base, ec);
        const fs::path projectFile = base / "Crashed.vbp";
        std::string saveError;
        daw::saveProjectFile(projectFile.string(), session.project(), saveError);
        fs::last_write_time(projectFile, fs::file_time_type::clock::now() - std::chrono::hours(1), ec);
        const double real = daw::unixSeconds();
        std::string file;
        const bool made = daw::writeAutosaveVersion((root / daw::autosaveKey(projectFile.string())).string(),
                                                    projectFile.string(), session.project(), std::floor(real), false, "", 10,
                                                    file, saveError);
        daw::SessionLock dead;
        dead.pid = 999999999LL; // no such process
        dead.started = real - 3600.0;
        dead.key = daw::autosaveKey(projectFile.string());
        dead.path = projectFile.string();
        std::string writeError;
        daw::writeSessionLockFile(root.string(), dead, writeError);
        const daw::Recoverable found = daw::findRecoverable(root.string(), real);
        c.check(made && found.found && found.path == projectFile.string() && found.name == session.project().name &&
                    found.autosave == (root / dead.key / file).string(),
                "Autosave: a crash is found with its newest autosave", found.name);

        // The file saved after the autosave: nothing to recover
        fs::last_write_time(projectFile, fs::file_time_type::clock::now() + std::chrono::minutes(10), ec);
        c.check(!daw::findRecoverable(root.string(), real).found,
                "Autosave: a project saved after its autosave is not offered");
        fs::last_write_time(projectFile, fs::file_time_type::clock::now() - std::chrono::hours(1), ec);

        // A session still running (this process's own lock) is not a crash
        dead.pid = daw::currentProcessId();
        daw::writeSessionLockFile(root.string(), dead, writeError);
        c.check(!daw::findRecoverable(root.string(), real).found,
                "Autosave: a running session is not a crash");

        // Only the lock's own process removes it
        daw::removeSessionLockFile(root.string(), 12345);
        c.check(fs::exists(root / "session.lock"), "Autosave: another process's lock is left alone");
        daw::removeSessionLockFile(root.string(), dead.pid);
        c.check(!fs::exists(root / "session.lock"), "Autosave: the owner's lock is removed");

        // Times as the lists show them: today, yesterday, an earlier day, and an earlier year
        const std::string todayText = daw::autosaveTimeText(real, real);
        const std::string yesterdayText = daw::autosaveTimeText(real - 86400.0, real);
        const std::string earlierText = daw::autosaveTimeText(real - 5 * 86400.0, real);
        const std::time_t realTime = static_cast<std::time_t>(real);
        const int year = std::localtime(&realTime)->tm_year + 1900;
        const std::string yearText = daw::autosaveTimeText(real, real + 400 * 86400.0);
        c.check(todayText.rfind("today ", 0) == 0 && todayText.size() == 11 && yesterdayText.rfind("yesterday ", 0) == 0 &&
                    earlierText.size() > 7 && earlierText[earlierText.size() - 3] == ':' &&
                    earlierText.find("today") == std::string::npos && yearText.find(std::to_string(year)) != std::string::npos,
                "Autosave: versions are timed as today, yesterday, a day or a year", todayText + " / " + earlierText);

        // Recovery declined: offered again only for an autosave newer than the decision
        c.check(!daw::recoveryDeclined(0.0, 100.0) && daw::recoveryDeclined(200.0, 150.0) &&
                    daw::recoveryDeclined(200.0, 200.0) && !daw::recoveryDeclined(200.0, 250.0),
                "Autosave: a declined recovery is offered again only for a newer autosave");
        std::string markerError;
        const std::string declinedFolder = (base / "declined").string();
        daw::writeDeclined(declinedFolder, 1700000123.5, markerError);
        c.check(daw::readDeclined(declinedFolder) == 1700000123.5 && daw::readDeclined((base / "none").string()) == 0.0,
                "Autosave: the declined time is kept in its session's folder", markerError);

        // Versions: a manual one and an automatic one, one deleted (its file and its entry)
        const std::string versionFolder = (base / "versions").string();
        daw::Project versionProject = daw::demoProject();
        std::string versionFile, versionError;
        const bool wrote1 = daw::writeAutosaveVersion(versionFolder, "", versionProject, 1700000300.0, true, "First idea", 10,
                                                      versionFile, versionError);
        const std::string firstFile = versionFile;
        const bool wrote2 = daw::writeAutosaveVersion(versionFolder, "", versionProject, 1700000400.0, false, "", 10,
                                                      versionFile, versionError);
        const size_t listed = daw::readManifest(versionFolder).versions.size();
        const bool deleted = daw::deleteVersion(versionFolder, firstFile, versionError);
        const daw::AutosaveManifest afterDelete = daw::readManifest(versionFolder);
        c.check(wrote1 && wrote2 && listed == 2 && deleted && afterDelete.versions.size() == 1 &&
                    afterDelete.versions[0].file == versionFile && !fs::exists(fs::path(versionFolder) / firstFile) &&
                    fs::exists(fs::path(versionFolder) / versionFile),
                "Autosave: a deleted version goes from its folder and its manifest", versionError);

        fs::remove_all(base, ec);
    }

    std::printf("\n%s: %d failed\n", c.failures == 0 ? "All passed" : "FAILED", c.failures);
    return c.failures == 0 ? 0 : 1;
}

}
