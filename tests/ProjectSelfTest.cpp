#include "ProjectSelfTest.h"
#include "Harness.h"

#include "BridgeSelfTest.h"
#include "RoutingSelfTest.h"
#include "MarkersSelfTest.h"
#include "MidiImportSelfTest.h"
#include "TakesSelfTest.h"
#include "FreezeSelfTest.h"
#include "WarpSelfTest.h"
#include "TempoSelfTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "audio/Effects.h"
#include "audio/PluginInstrument.h"
#include "audio/Synth.h"
#include "daw/app/Appearance.h"
#include "daw/audio/AutomationWriter.h"
#include "daw/io/Autosave.h"
#include "daw/model/ExportModel.h"
#include "daw/model/ExportModel.h"
#include "daw/model/History.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/ProjectSession.h"
#include "daw/model/ProjectStore.h"
#include "daw/io/Toml.h"

namespace tests {

namespace {

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
    odd.timing = audio::TempoMap(97.25, 3);
    odd.bars = 7;
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
    bool exact = oddOk && back.name == odd.name && back.timing == odd.timing && back.bars == 7 &&
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
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "vybn-audio-selftest";
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
        p.timing.setTempo(120.0); // half a second per beat
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

    // Moving a track (dragging its header): clips and ids go with it, the selection stays on the same track
    {
        daw::Project p = daw::demoProject(); // Guitar, Bass, Lead, Drums, Vocals, Ambience
        const daw::Project before = p;
        auto order = [](const daw::Project &project) {
            std::string initials;
            for (const daw::Track &t : project.tracks)
                initials += t.name[0];
            return initials;
        };
        const daw::ObjectId guitarId = p.tracks[0].id;
        const size_t guitarClips = p.tracks[0].clips.size();
        p.select(0, 1);
        p.moveTrack(0, 3);
        c.check(order(p) == "BLDGVA" && p.tracks[3].id == guitarId && p.tracks[3].clips.size() == guitarClips &&
                    p.selectedTrack == 3 && p.selectedClip == 1,
                "Moving a track keeps its clips and ids, and the selection follows it", order(p));
        p.moveTrack(3, 0);
        bool sameIds = p.tracks.size() == before.tracks.size();
        for (size_t i = 0; sameIds && i < p.tracks.size(); ++i)
            sameIds = p.tracks[i].id == before.tracks[i].id;
        c.check(order(p) == "GBLDVA" && sameIds && p.selectedTrack == 0, "Moving it back restores the order");

        // A track passed over shifts by one, in either direction; the others don't move
        p.select(2); // Lead
        p.moveTrack(0, 4);
        const int afterDown = p.selectedTrack;
        p.moveTrack(5, 0);
        c.check(afterDown == 1 && p.selectedTrack == 2 && p.tracks[static_cast<size_t>(p.selectedTrack)].name == "Lead",
                "A selected track that is passed over stays selected as others move across it");
        p.select(1);
        const std::string ordered = order(p);
        p.moveTrack(-1, 2);
        p.moveTrack(2, 99);
        p.moveTrack(2, 2);
        c.check(order(p) == ordered && p.selectedTrack == 1, "A move to or from a track that isn't there, or to its own place, does nothing");

        daw::Project moved = before;
        moved.moveTrack(0, 2);
        c.check(daw::describeChange(before, moved) == "Move track" && !daw::sameContent(before, moved),
                "History: moving a track is named and is a change", daw::describeChange(before, moved));
    }

    // Through a file
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "vybn-project-selftest.vbp";
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
    bool handRight = handOk && h.name == "Literal \\ string" && h.timing.tempo() == 120.0 && h.bars == 16 && h.tracks.size() == 2;
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
            p.timing.setTempo(60.0 + i);
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
        const bool exact = older && older->timing.tempo() == 60.0 + 248 && older->tracks.size() == p.tracks.size() &&
                           daw::sameContent(*older, [&] {
                               daw::Project q = p;
                               q.timing.setTempo(60.0 + 248);
                               return q;
                           }()) &&
                           daw::sameSelection(*older, p);
        c.check(stored == clipCount && exact, "History: steps share the clips that didn't change",
                std::to_string(stored) + " clips stored for " + std::to_string(clipCount));
        p = *older;
        const daw::Project *newer = h.redo(p);
        c.check(newer && newer->timing.tempo() == 60.0 + 249 && daw::sameSelection(*newer, p), "History: redo after sharing");
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
        synth.name = "VYBN Synth";
        synth.setValue(0, 3.0);     // Osc 1 wave: Square
        synth.setValue(34, -40.0);  // filter envelope amount: bipolar
        synth.setValue(31, 1234.5); // cutoff
        synth.bypass = false;
        p.tracks[0].instrument = synth;
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool ok = daw::readProject(text, back, error);
        c.check(ok && back.tracks[0].instrument == synth && back.tracks[0].instrument.name == "VYBN Synth" &&
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
        c.check(from == 0.0 && to == std::max(last, p.timing.barLength(0)), "Export: a song runs to its last clip",
                "to " + std::to_string(to) + " beats");
    }

    // The document: saved state, the file, and the history
    {
        daw::ProjectSession s(daw::demoProject());
        c.check(!s.modified() && s.path().empty(), "Session: a new session is saved and has no file");
        const double tempo = s.project().timing.tempo();
        s.store().setTiming(audio::TempoMap(tempo + 12.0));
        s.checkModified();
        c.check(s.modified(), "Session: an edit makes it modified");
        s.markSaved("C:/Mixes/Demo.vbp");
        c.check(!s.modified() && s.path() == "C:/Mixes/Demo.vbp", "Session: saving clears modified and sets the file");
        s.store().setTiming(audio::TempoMap(tempo));
        s.checkModified();
        c.check(s.modified(), "Session: changing back after a save is modified again");
        s.store().setTiming(audio::TempoMap(tempo + 12.0));
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

        const double tempo = store.project().timing.tempo();
        r0 = store.revision();
        store.setTiming(audio::TempoMap(tempo));
        const bool tempoSame = store.revision() == r0;
        store.setTiming(audio::TempoMap(tempo + 7.0));
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
        store.setTiming(store.project().timing);
        c.check(store.revision() >= revAfter && store.structureRevision() >= structAfter && store.trackRevision(0) >= trackAfter,
                "Store: revisions only increase");
    }

    // Idle frames: on a big project the session's revision checks cost nothing per frame (the old way,
    // comparing the whole project each frame, took ~50 ms on 160,000 notes). 8 clips a track in the
    // fast tier (32,000 notes), 40 in the full one.
    {
        const int clipsPerTrack = full() ? 40 : 8;
        daw::Project big;
        for (int t = 0; t < 16; ++t) {
            const int index = big.addTrack(daw::TrackKind::Instrument);
            daw::Track &track = big.tracks[static_cast<size_t>(index)];
            track.clips.clear();
            for (int k = 0; k < clipsPerTrack; ++k) {
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
        // The guard: an idle frame under 5 ms. Scanning the project each frame took ~50 ms at 160,000
        // notes, and grows with the project; revisions cost the same on any size
        char cost[96];
        std::snprintf(cost, sizeof cost, "%.4f ms a frame, %zu notes", revisionMs / frames, notes);
        c.check(revisionMs / frames < 5.0, "Idle frames: the revision checks cost nothing per frame on a big project", cost);
        const std::string savedText = daw::writeProject(big);

        // An edit after the idle frames is recorded once it settles
        const size_t before = session.history().undoCount();
        session.store().editClip(0, 0).name = "Idle edit";
        const bool waited = !session.recordHistory(false, false, 2.0);
        const bool recorded = session.recordHistory(true, false, 2.1);
        c.check(waited && recorded && session.history().undoCount() == before + 1 &&
                    session.history().undoLabel() == "Rename clip",
                "Idle frames: an edit after them is recorded once settled", session.history().undoLabel());

        // One saved-state check after an edit agrees with writing the whole project and comparing its
        // text. The name goes back, and the edit is at the very end of the project, so the compare
        // goes through everything before it
        session.store().editClip(0, 0).name = "Clip 0";
        session.store().editClip(15, clipsPerTrack - 1).notes.back().velocity = 0.3f;
        const Clock::time_point checkStart = Clock::now();
        session.checkModified();
        const double checkMs = elapsed(checkStart);
        const bool textDiffers = daw::writeProject(session.project()) != savedText;
        c.check(session.modified() && textDiffers, "Saved state: the check after an edit agrees with the text");
        if (bench())
            std::printf("INFO idle frame: %.4f ms with revisions (first frame %.1f ms; %zu notes); one check after an edit: %.3f ms\n",
                        revisionMs / frames, firstMs, notes, checkMs);
    }

    // Undo: a single-note edit comes back exactly (and goes again on redo), and arming, monitoring and the
    // loop stay as they are now. With the benchmark on, also what undo costs on a big project: these numbers
    // are the baseline the undo redesign is measured against (the churn is reported, not asserted).
    {
        using Clock = std::chrono::steady_clock;
        auto elapsed = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };
        // `tracks` instrument tracks of `clips` clips of `notes` notes each
        auto build = [](int tracks, int clips, int notes) {
            daw::Project p;
            for (int t = 0; t < tracks; ++t) {
                const int index = p.addTrack(daw::TrackKind::Instrument);
                daw::Track &track = p.tracks[static_cast<size_t>(index)];
                track.clips.clear();
                for (int k = 0; k < clips; ++k) {
                    std::vector<daw::Note> list(static_cast<size_t>(notes));
                    for (size_t n = 0; n < list.size(); ++n) {
                        list[n].start = static_cast<double>(n) * 0.0625;
                        list[n].length = 0.25;
                        list[n].key = 40 + static_cast<int>(n % 40);
                        list[n].velocity = 0.7f;
                    }
                    track.clips.push_back(daw::Clip("Clip " + std::to_string(k), k * 8.0, 8.0, std::move(list)));
                }
            }
            return p;
        };
        // One undo or redo as DawApp does it: the history's plan, put back by the session
        auto undoStep = [](daw::ProjectSession &s, double now) {
            daw::HistoryRestore plan;
            if (!s.history().undo(s.store(), now, plan))
                return false;
            s.restoreState(std::move(plan));
            return true;
        };
        auto redoStep = [](daw::ProjectSession &s) {
            daw::HistoryRestore plan;
            if (!s.history().redo(s.store(), plan))
                return false;
            s.restoreState(std::move(plan));
            return true;
        };

        // The checks, on a small project
        {
            daw::ProjectSession s(build(4, 3, 8));
            s.history().reset(s.project());
            s.recordHistory(true, false, 0.0);
            const daw::Project original = s.project();
            const double was = original.tracks[2].clips[1].notes[3].start;
            s.store().editClip(2, 1).notes[3].start = was + 1.0;
            s.recordHistory(true, false, 1.0);
            const daw::Project edited = s.project();
            // Arming, monitoring and the loop change after the step was recorded
            s.store().editMix(1).armed = true;
            s.store().editMix(1).monitor = true;
            s.store().setLoop(2.0, 6.0, true);
            const bool undone = undoStep(s, 2.0);
            c.check(undone && s.project().tracks[2].clips[1].notes[3].start == was && daw::sameContent(s.project(), original),
                    "Undo: a single-note edit is put back exactly");
            c.check(s.project().tracks[1].armed && s.project().tracks[1].monitor && s.project().looping &&
                        s.project().loopStart == 2.0 && s.project().loopEnd == 6.0,
                    "Undo: arming, monitoring and the loop are kept as they are now");
            const bool redone = redoStep(s);
            c.check(redone && s.project().tracks[2].clips[1].notes[3].start == was + 1.0 && daw::sameContent(s.project(), edited),
                    "Undo: redo applies the edit again");
            c.check(s.project().tracks[1].armed && s.project().tracks[1].monitor && s.project().looping,
                    "Undo: redo keeps arming, monitoring and the loop too");
        }

        // The history against a plain one that copies the whole project for its state and every step (how
        // it was before it followed the store's revisions): a few hundred random edits, settled and not,
        // must give the same steps, labels, merges, and undo and redo results with the same selection
        {
            struct Plain {
                struct Step {
                    daw::Project project;
                    std::string label;
                };
                daw::Project state;
                std::vector<Step> undo, redo;
                double lastTime = -1e9;
                bool update(const daw::Project &project, bool settled, bool canMerge, double now)
                {
                    if (daw::sameContent(project, state)) {
                        if (!daw::sameSelection(project, state))
                            state = project;
                        return false;
                    }
                    if (!settled)
                        return false;
                    std::string label = daw::describeChange(state, project);
                    const bool merge = canMerge && !undo.empty() && undo.back().label == label && now - lastTime < 0.5;
                    if (!merge)
                        undo.push_back({state, std::move(label)});
                    redo.clear();
                    state = project;
                    lastTime = now;
                    return true;
                }
                const daw::Project *back(const daw::Project &current, double now)
                {
                    update(current, true, false, now);
                    if (undo.empty())
                        return nullptr;
                    Step step = std::move(undo.back());
                    undo.pop_back();
                    redo.push_back({state, step.label});
                    state = step.project;
                    lastTime = -1e9;
                    return &state;
                }
                const daw::Project *forward(const daw::Project &current)
                {
                    if (redo.empty() || !daw::sameContent(current, state))
                        return nullptr;
                    Step step = std::move(redo.back());
                    redo.pop_back();
                    undo.push_back({current, step.label});
                    state = step.project;
                    lastTime = -1e9;
                    return &state;
                }
            };

            const int rounds = full() ? 6 : 2, edits = full() ? 190 : 150;
            std::string problem;
            size_t steps = 0, merges = 0, undone = 0;
            for (int round = 0; round < rounds && problem.empty(); ++round) {
                std::mt19937 rng(1234u + static_cast<unsigned>(round));
                auto pick = [&rng](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
                daw::ProjectSession fs(build(4, 3, 6));
                fs.history().reset(fs.project());
                daw::History viaProject;
                viaProject.reset(fs.project());
                Plain plain;
                plain.state = fs.project();
                const daw::Project first = fs.project();
                double now = 1.0;
                auto flag = [&problem](const std::string &what) {
                    if (problem.empty())
                        problem = what;
                };
                auto agree = [&](const char *what) {
                    daw::History &h = fs.history();
                    if (h.undoCount() != plain.undo.size() || viaProject.undoCount() != plain.undo.size() ||
                        h.redoCount() != plain.redo.size() || viaProject.redoCount() != plain.redo.size())
                        flag(std::string("step counts after ") + what);
                    else if (!plain.undo.empty() && (h.undoLabel() != plain.undo.back().label ||
                                                    viaProject.undoLabel() != plain.undo.back().label))
                        flag(std::string("labels after ") + what + ": " + h.undoLabel() + " / " + plain.undo.back().label);
                };
                auto frame = [&](bool settled, bool merge) {
                    fs.store().normalizeIds();
                    fs.verify();
                    const size_t before = fs.history().undoCount();
                    const bool a = fs.recordHistory(settled, merge, now);
                    const bool b = viaProject.update(fs.project(), settled, merge, now);
                    const bool n = plain.update(fs.project(), settled, merge, now);
                    if (a != n || b != n)
                        flag("recorded or not");
                    if (a && fs.history().undoCount() == before)
                        merges++;
                    agree("a frame");
                };
                // One undo or redo through all three, the result put back in the session as DawApp does it
                auto move = [&](bool redo) {
                    daw::HistoryRestore plan;
                    const bool x = redo ? fs.history().redo(fs.store(), plan) : fs.history().undo(fs.store(), now, plan);
                    const daw::Project *y = redo ? viaProject.redo(fs.project()) : viaProject.undo(fs.project(), now);
                    const daw::Project *z = redo ? plain.forward(fs.project()) : plain.back(fs.project(), now);
                    if (x != (z != nullptr) || (y != nullptr) != (z != nullptr)) {
                        flag("undo or redo available");
                        return false;
                    }
                    if (!z)
                        return false;
                    // Through the store's own restore: the project must come out as the plain history's state,
                    // with arming, monitoring and the loop as they were
                    const daw::Project was = fs.project();
                    fs.restoreState(std::move(plan));
                    const daw::Project &now2 = fs.project();
                    if (!daw::sameContent(now2, *z) || !daw::sameSelection(now2, *z) || !daw::sameContent(*y, *z) ||
                        !daw::sameSelection(*y, *z))
                        flag(std::string(redo ? "redo" : "undo") + " result");
                    for (const daw::Track &t : now2.tracks) {
                        const daw::Track *before = was.findTrack(t.id);
                        if (before && before->kind == t.kind && (before->armed != t.armed || before->monitor != t.monitor))
                            flag("arming or monitoring changed by an undo");
                    }
                    if (now2.loopStart != was.loopStart || now2.loopEnd != was.loopEnd || now2.looping != was.looping)
                        flag("the loop changed by an undo");
                    fs.verify();
                    undone += redo ? 0 : 1;
                    agree("undo or redo");
                    return true;
                };

                for (int i = 0; i < edits && problem.empty(); ++i) {
                    daw::ProjectStore &store = fs.store();
                    const daw::Project &p = fs.project();
                    const int tracks = static_cast<int>(p.tracks.size());
                    const int t = pick(tracks);
                    const daw::Track &track = p.tracks[static_cast<size_t>(t)];
                    const int clips = static_cast<int>(track.clips.size());
                    const int k = clips > 0 ? pick(clips) : -1;
                    const int notes = k >= 0 ? static_cast<int>(track.clips[static_cast<size_t>(k)].notes.size()) : 0;
                    const int op = pick(19);
                    if (op <= 1 && notes > 0)
                        store.editClip(t, k).notes[static_cast<size_t>(pick(notes))].start += 0.125;
                    else if (op == 2 && k >= 0)
                        store.editClip(t, k).notes.push_back(daw::Note{pick(8) * 0.5, 0.5, 40 + pick(20), 0.8f, false, false});
                    else if (op == 3 && notes > 0)
                        store.editClip(t, k).notes.erase(store.editClip(t, k).notes.begin() + pick(notes));
                    else if (op == 4 && notes > 0) {
                        daw::Note &note = store.editClip(t, k).notes[static_cast<size_t>(pick(notes))];
                        note.selected = !note.selected;
                    } else if (op == 5)
                        store.editMix(t).gain = 0.5f + 0.1f * static_cast<float>(pick(10));
                    else if (op == 6)
                        store.editMix(t).armed = !track.armed;
                    else if (op == 7) {
                        std::vector<audio::DeviceSetting> &fx = store.editTrack(t).effects;
                        if (fx.size() < 3 && pick(3) > 0)
                            fx.push_back(audio::effectSetting(audio::EffectType::Gain));
                        else if (!fx.empty() && pick(2) == 0)
                            fx.pop_back();
                        else if (!fx.empty())
                            fx.back().bypass = !fx.back().bypass;
                    } else if (op == 8 || op == 9)
                        store.select(t, k >= 0 && pick(3) > 0 ? k : -1);
                    else if (op == 10 && tracks > 1)
                        store.edit().moveTrack(t, pick(tracks));
                    else if (op == 11) {
                        if (tracks < 6)
                            store.edit().addTrack(pick(2) ? daw::TrackKind::Instrument : daw::TrackKind::Audio);
                        else
                            store.edit().deleteTrack(t);
                    } else if (op == 12)
                        store.setTiming(audio::TempoMap(100.0 + 10.0 * pick(4)));
                    else if (op == 13 && k >= 0)
                        store.editClip(t, k).name = "Clip " + std::to_string(pick(5));
                    else if (op == 14 && k >= 0)
                        store.editTrack(t).clips.push_back(daw::Clip("New", pick(4) * 8.0, 8.0));
                    else if (op == 15 && clips > 1)
                        store.editTrack(t).clips.erase(store.editTrack(t).clips.begin() + k);
                    else if (op == 17)
                        store.editMix(t).monitor = !track.monitor;
                    else if (op == 18)
                        store.setLoop(pick(8), 8.0 + pick(8), pick(2) == 0);
                    else if (op == 16 && i % 5 == 0) {
                        // Some frames pass before an undo, as when the mouse is still held
                        frame(false, false);
                        move(pick(3) == 0);
                        continue;
                    }
                    now += pick(4) == 0 ? 0.1 : 1.0;
                    frame(pick(3) != 0, pick(2) == 0);
                }
                now += 1.0;
                frame(true, false);
                steps += fs.history().undoCount();
                // All the way back, then all the way forward again, each step the same in all three
                std::vector<daw::Project> seen;
                while (problem.empty() && move(false)) {
                }
                const bool backToFirst = daw::sameContent(fs.project(), first);
                if (!backToFirst)
                    flag("undoing everything did not reach the first project");
                while (problem.empty() && move(true)) {
                }
            }
            c.check(problem.empty(), "History fuzz: the same steps, labels, merges and results as copying the whole project",
                    problem.empty() ? std::to_string(steps) + " steps, " + std::to_string(undone) + " undos, " +
                                          std::to_string(merges) + " merges"
                                    : problem);
        }

        // What an undo moves: a one-note edit on one track moves that track's content revision only, a gain
        // change that track's mix revision only, and moving a track takes the structural path (replace)
        {
            daw::ProjectSession cs(build(6, 3, 8));
            cs.history().reset(cs.project());
            cs.recordHistory(true, false, 0.0);
            struct Moved {
                std::vector<int> tracks, mix;
                bool structure = false, master = false, selection = false, revision = false, structural = false, any = false;
            };
            // One undo or redo through the session, and which revisions it moved
            auto apply = [&cs](bool redo, double now) {
                const daw::ProjectStore &st = cs.store();
                const int n = static_cast<int>(st.project().tracks.size());
                std::vector<uint64_t> t, m;
                for (int i = 0; i < n; ++i) {
                    t.push_back(st.trackRevision(i));
                    m.push_back(st.mixRevision(i));
                }
                const uint64_t structure = st.structureRevision(), master = st.masterRevision(),
                               selection = st.selectionRevision(), revision = st.revision();
                Moved out;
                daw::HistoryRestore plan;
                out.any = redo ? cs.history().redo(cs.store(), plan) : cs.history().undo(cs.store(), now, plan);
                out.structural = plan.structural;
                cs.restoreState(std::move(plan));
                for (int i = 0; i < n && i < static_cast<int>(t.size()); ++i) {
                    if (st.trackRevision(i) != t[static_cast<size_t>(i)])
                        out.tracks.push_back(i);
                    if (st.mixRevision(i) != m[static_cast<size_t>(i)])
                        out.mix.push_back(i);
                }
                out.structure = st.structureRevision() != structure;
                out.master = st.masterRevision() != master;
                out.selection = st.selectionRevision() != selection;
                out.revision = st.revision() != revision;
                return out;
            };
            const std::vector<int> only2{2}, only4{4}, none;

            cs.store().editClip(2, 1).notes[3].start += 1.0;
            cs.recordHistory(true, false, 1.0);
            Moved m = apply(false, 2.0);
            c.check(m.any && !m.structural && m.tracks == only2 && m.mix == none && !m.structure && !m.master && !m.selection &&
                        m.revision,
                    "Undo churn: a one-note edit on one track moves only that track's content");
            m = apply(true, 0.0);
            c.check(m.any && !m.structural && m.tracks == only2 && m.mix == none && !m.structure && !m.master && !m.selection &&
                        m.revision,
                    "Undo churn: redo moves only that track's content too");
            cs.store().editMix(4).gain = 0.3f;
            cs.recordHistory(true, false, 3.0);
            m = apply(false, 4.0);
            c.check(m.any && !m.structural && m.tracks == none && m.mix == only4 && !m.structure && !m.master && !m.selection,
                    "Undo churn: a gain change moves only that track's mix");
            cs.store().edit().moveTrack(0, 3);
            cs.recordHistory(true, false, 5.0);
            m = apply(false, 6.0);
            c.check(m.any && m.structural && m.structure, "Undo churn: moving a track takes the structural path");
            // The frame after an undo has nothing to compare, and the next edit is recorded as usual
            const size_t steps = cs.history().undoCount();
            cs.store().editClip(1, 0).notes[0].velocity = 0.1f;
            const bool recorded = cs.recordHistory(true, false, 7.0);
            c.check(recorded && cs.history().undoCount() == steps + 1 && cs.history().undoLabel() == "Change velocity",
                    "Undo churn: an edit after an undo is recorded", cs.history().undoLabel());
            cs.verify();
        }

        // A press that selects a clip and then drags it: undo puts back the selection from the press
        {
            daw::ProjectSession ds(build(3, 2, 6));
            ds.history().reset(ds.project());
            ds.recordHistory(true, false, 0.0);
            const double was = ds.project().tracks[0].clips[1].notes[2].start;
            ds.store().select(0, 1); // the press, the mouse down
            ds.recordHistory(false, false, 1.0);
            for (int f = 0; f < 4; ++f) {
                ds.store().editClip(0, 1).notes[2].start += 0.25; // the drag
                ds.recordHistory(false, false, 1.1 + f * 0.05);
            }
            const bool recorded = ds.recordHistory(true, false, 2.0); // the mouse up
            const daw::Project *back = ds.history().undo(ds.project(), 3.0);
            c.check(recorded && ds.history().undoCount() == 0 && back && back->selectedTrack == 0 && back->selectedClip == 1 &&
                        back->tracks[0].clips[1].notes[2].start == was,
                    "History: undoing a drag that began with a selection puts back that selection");
        }

        if (bench()) {
            const int tracks = 32, clips = 20, notes = 64;
            daw::ProjectSession s(build(tracks, clips, notes));
            s.history().reset(s.project());
            s.recordHistory(true, false, 0.0);
            double now = 1.0;

            // a. A drag: a small edit each frame, unsettled, then the settled frame. The history stops comparing
            // at the first difference, so a drag on the first track and one on the last are timed apart
            const int dragFrames = 200;
            double dragMs = 0.0, dragFirstMs = 0.0;
            for (int f = 0; f < dragFrames; ++f) {
                s.store().editClip(0, 0).notes[5].start += 0.001;
                const Clock::time_point from = Clock::now();
                s.recordHistory(false, false, now);
                dragFirstMs += elapsed(from);
                now += 1.0 / 60.0;
            }
            s.recordHistory(true, false, now);
            for (int f = 0; f < dragFrames; ++f) {
                s.store().editClip(tracks - 1, clips - 1).notes[5].start += 0.001;
                const Clock::time_point from = Clock::now();
                s.recordHistory(false, false, now);
                dragMs += elapsed(from);
                now += 1.0 / 60.0;
            }
            const Clock::time_point settleFrom = Clock::now();
            s.recordHistory(true, false, now);
            const double settledMs = elapsed(settleFrom);
            now += 1.0;

            // b. Selection changes: each one a different track and clip
            const int selects = 200;
            double selectMs = 0.0;
            for (int i = 0; i < selects; ++i) {
                s.store().select(i % tracks, (i / tracks) % clips);
                const Clock::time_point from = Clock::now();
                s.recordHistory(true, false, now);
                selectMs += elapsed(from);
                now += 1.0;
            }

            // e. Memory: 50 single-note edits on the same clip (their steps are what c undoes)
            const int edits = 50;
            for (int i = 0; i < edits; ++i) {
                s.store().editClip(11, 4).notes[10].start += 0.01;
                s.recordHistory(true, false, now);
                now += 1.0;
            }
            const size_t stored = s.history().storedClips();
            // Rough bytes: the clips held, each with its notes, plus a shell (the project without its clips) per step
            const size_t clipBytes = sizeof(daw::Clip) + static_cast<size_t>(notes) * sizeof(daw::Note);
            const size_t shellBytes = sizeof(daw::Project) + static_cast<size_t>(tracks) * sizeof(daw::Track);
            const size_t bytes = stored * clipBytes + (s.history().undoCount() + s.history().redoCount()) * shellBytes;
            const size_t held = s.history().storedBytes();

            // d. Churn: one single-note edit on one track, recorded, then its undo; what the undo moved
            s.store().editClip(20, 2).notes[1].velocity = 0.2f;
            s.recordHistory(true, false, now);
            now += 1.0;
            std::vector<uint64_t> trackRev, mixRev;
            for (int t = 0; t < tracks; ++t) {
                trackRev.push_back(s.store().trackRevision(t));
                mixRev.push_back(s.store().mixRevision(t));
            }
            const uint64_t structure = s.store().structureRevision(), master = s.store().masterRevision(),
                           selection = s.store().selectionRevision();
            undoStep(s, now);
            int tracksMoved = 0, mixMoved = 0;
            for (int t = 0; t < tracks; ++t) {
                tracksMoved += s.store().trackRevision(t) != trackRev[static_cast<size_t>(t)] ? 1 : 0;
                mixMoved += s.store().mixRevision(t) != mixRev[static_cast<size_t>(t)] ? 1 : 0;
            }
            const int structureMoved = s.store().structureRevision() != structure ? 1 : 0;
            const int masterMoved = s.store().masterRevision() != master ? 1 : 0;
            const int selectionMoved = s.store().selectionRevision() != selection ? 1 : 0;
            // The frame after an undo: the history looks at the restored project once
            const Clock::time_point afterFrom = Clock::now();
            s.recordHistory(true, false, now);
            const double afterMs = elapsed(afterFrom);

            // c. Undo and redo latency: the history's step plus the session's restore, over the single-note steps
            double undoMs = 0.0, redoMs = 0.0;
            int undos = 0, redos = 0;
            for (int i = 0; i < edits; ++i) {
                const Clock::time_point from = Clock::now();
                undos += undoStep(s, now) ? 1 : 0;
                undoMs += elapsed(from);
            }
            for (int i = 0; i < edits; ++i) {
                const Clock::time_point from = Clock::now();
                redos += redoStep(s) ? 1 : 0;
                redoMs += elapsed(from);
            }

            std::printf("INFO undo bench (%dx%dx%d, %zu notes): drag frame %.4f ms (first track %.4f ms), settled frame %.3f ms, selection %.4f ms, "
                        "undo %.3f ms, redo %.3f ms, after-undo frame %.3f ms\n",
                        tracks, clips, notes, noteCount(s.project()), dragMs / dragFrames, dragFirstMs / dragFrames, settledMs, selectMs / selects,
                        undoMs / std::max(undos, 1), redoMs / std::max(redos, 1), afterMs);
            std::printf("INFO undo churn: tracks %d/%d, mix %d/%d, structure %d, master %d, selection %d; "
                        "stored clips %zu after %d edits, ~%zu bytes (%zu by the old estimate)\n",
                        tracksMoved, tracks, mixMoved, tracks, structureMoved, masterMoved, selectionMoved, stored, edits, held, bytes);
        }
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
        daw::ExportTarget p;
        p.name = "Share";
        p.format = daw::ExportFormat::Mp3;
        p.bits = 16;
        p.bitrate = 192;
        p.sampleRate = 44100;
        tp.exportPresets = {{"Everything \"quoted\"", {a, b, d, e, m, p}}, {"Empty", {}}};

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

        // An MP3 target read with what MP3 can't take: the nearest rate, 16 bits, a standard bitrate
        const std::string wildMp3 = "[[export_presets]]\nname = \"Wild\"\n\n[[export_presets.targets]]\nformat = \"mp3\"\n"
                                    "bits = 24\nbitrate = 300\nsample_rate = 88200\n";
        daw::Project mp3P;
        std::string mp3Why;
        const bool mp3Ok = daw::readProject(wildMp3, mp3P, mp3Why);
        const daw::ExportTarget *w3 = mp3Ok && !mp3P.exportPresets.empty() && !mp3P.exportPresets[0].targets.empty()
                                          ? &mp3P.exportPresets[0].targets[0]
                                          : nullptr;
        c.check(w3 && w3->format == daw::ExportFormat::Mp3 && w3->bits == 16 && w3->bitrate == 320 && w3->sampleRate == 44100,
                "Export: an MP3 target is read at a rate, depth and bitrate MP3 can take", mp3Why);
        c.check(daw::mp3RateFor(22050) == 44100 && daw::mp3RateFor(32000) == 32000 && daw::mp3RateFor(48000) == 48000 &&
                    daw::mp3RateFor(176400) == 44100 && daw::mp3RateFor(192000) == 48000,
                "Export: an MP3's rate keeps the family (44.1 kHz or 48 kHz)");
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
        t.format = daw::ExportFormat::Mp3;
        t.bitrate = 256;
        t.sampleRate = 96000;
        t.fileName = "{project} {rate} {bits} {format}";
        const std::string n15 = daw::resolveFileName(t, np, "Mix", "", now);
        c.check(n15 == "Night Mix 48k 256kbps mp3.mp3", "File names: MP3 is .mp3, with its rate and bitrate", n15);
    }

    // The part of the song each range covers
    {
        daw::Project rp;
        rp.bars = 16;
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
        const fs::path base = fs::temp_directory_path() / "vybn-autosave-selftest";
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

    // Stable ids: every track and clip has a unique non-zero id, kept through edits, saving and opening
    {
        auto trackIds = [](const daw::Project &p) {
            std::vector<daw::ObjectId> ids;
            for (const daw::Track &t : p.tracks)
                ids.push_back(t.id);
            return ids;
        };
        auto clipIds = [](const daw::Project &p) {
            std::vector<daw::ObjectId> ids;
            for (const daw::Track &t : p.tracks) {
                for (const daw::Clip &cl : t.clips)
                    ids.push_back(cl.id);
            }
            return ids;
        };
        // No id is 0 and no two are alike
        auto distinctNonZero = [](std::vector<daw::ObjectId> ids) {
            if (std::find(ids.begin(), ids.end(), daw::kNoId) != ids.end())
                return false;
            std::sort(ids.begin(), ids.end());
            return std::adjacent_find(ids.begin(), ids.end()) == ids.end();
        };
        // The highest track or clip id in a project
        auto highest = [](const daw::Project &p) {
            daw::ObjectId top = daw::kNoId;
            for (const daw::Track &t : p.tracks) {
                top = std::max(top, t.id);
                for (const daw::Clip &cl : t.clips)
                    top = std::max(top, cl.id);
            }
            return top;
        };
        // Two tracks (the first with two clips, the second with one) with the given ids
        auto shape = [](daw::ObjectId t0, daw::ObjectId t1, daw::ObjectId c0, daw::ObjectId c1, daw::ObjectId c2) {
            daw::Project p;
            p.tracks.resize(2);
            p.tracks[0].id = t0;
            p.tracks[1].id = t1;
            p.tracks[0].clips = {daw::Clip("A", 0.0, 4.0), daw::Clip("B", 4.0, 4.0)};
            p.tracks[0].clips[0].id = c0;
            p.tracks[0].clips[1].id = c1;
            p.tracks[1].clips = {daw::Clip("C", 0.0, 4.0)};
            p.tracks[1].clips[0].id = c2;
            return p;
        };

        // normalizeIds on ids all 0: every track and clip gets a unique non-zero id; a second call changes nothing
        daw::Project z = shape(0, 0, 0, 0, 0);
        daw::ObjectId next = 1;
        const bool zeroChanged = z.normalizeIds(next);
        c.check(zeroChanged && distinctNonZero(trackIds(z)) && distinctNonZero(clipIds(z)),
                "Ids: zero ids become unique non-zero ids", std::to_string(trackIds(z).size()) + " tracks");
        c.check(next > highest(z), "Ids: next ends above every id given");
        const std::vector<daw::ObjectId> zTracks = trackIds(z), zClips = clipIds(z);
        const bool zAgain = z.normalizeIds(next);
        c.check(!zAgain && trackIds(z) == zTracks && clipIds(z) == zClips, "Ids: a second normalizeIds changes nothing");

        // Unique ids are kept exactly
        daw::Project k = shape(3, 9, 4, 6, 10);
        daw::ObjectId nextK = 1;
        const bool keptChanged = k.normalizeIds(nextK);
        c.check(!keptChanged && trackIds(k) == std::vector<daw::ObjectId>({3, 9}) && clipIds(k) == std::vector<daw::ObjectId>({4, 6, 10}),
                "Ids: unique ids are kept exactly");

        // A duplicated track id: the first keeps it, the later gets a new one
        daw::Project dt = shape(3, 3, 4, 6, 10);
        daw::ObjectId nextT = 1;
        const bool dupTrackChanged = dt.normalizeIds(nextT);
        c.check(dupTrackChanged && dt.tracks[0].id == 3 && dt.tracks[1].id != 3 && distinctNonZero(trackIds(dt)) && nextT > highest(dt),
                "Ids: a duplicate track id is renumbered after the first");

        // A clip copied into another track (same id): the first keeps it, and next ends above every id
        daw::Project dc = shape(3, 9, 4, 6, 4);
        daw::ObjectId nextC = 1;
        const bool dupClipChanged = dc.normalizeIds(nextC);
        c.check(dupClipChanged && dc.tracks[0].clips[0].id == 4 && dc.tracks[1].clips[0].id != 4 && distinctNonZero(clipIds(dc)) &&
                    nextC > highest(dc),
                "Ids: a duplicate clip id is renumbered after the first");

        // A track and a clip may share a number: the namespaces are separate
        daw::Project ns;
        ns.tracks.resize(1);
        ns.tracks[0].id = 5;
        ns.tracks[0].clips = {daw::Clip("D", 0.0, 4.0)};
        ns.tracks[0].clips[0].id = 5;
        daw::ObjectId nextNs = 1;
        const bool nsChanged = ns.normalizeIds(nextNs);
        c.check(!nsChanged && ns.tracks[0].id == 5 && ns.tracks[0].clips[0].id == 5, "Ids: a track and a clip may share a number");

        // Lookups by id: the index or the track, -1 or nullptr for a missing id and for kNoId
        daw::Project f = shape(10, 20, 1, 2, 3);
        f.tracks.push_back(daw::Track()); // id 0, which kNoId must not find
        const daw::Project &cf = f;
        c.check(f.trackIndex(20) == 1 && cf.trackIndex(10) == 0 && f.trackIndex(30) == -1 && f.trackIndex(daw::kNoId) == -1 &&
                    f.findTrack(20) == &f.tracks[1] && cf.findTrack(10) == &f.tracks[0] && f.findTrack(30) == nullptr &&
                    cf.findTrack(daw::kNoId) == nullptr,
                "Ids: tracks are found by id, none for a missing id or kNoId");

        // A store made from a project with zero ids has non-zero ids at once
        daw::ProjectStore zs(shape(0, 0, 0, 0, 0));
        c.check(distinctNonZero(trackIds(zs.project())) && distinctNonZero(clipIds(zs.project())),
                "Ids: a store gives its project ids on construction");

        // normalizeIds with nothing to do moves no revision
        const uint64_t nr = zs.revision(), ns0 = zs.structureRevision(), nt = zs.trackRevision(0);
        const bool noop = zs.normalizeIds();
        c.check(!noop && zs.revision() == nr && zs.structureRevision() == ns0 && zs.trackRevision(0) == nt,
                "Ids: a store's normalizeIds with nothing to do moves nothing");

        // A clip with id 0 added by an edit: normalizeIds gives it an id, moving the revisions but not the structure
        daw::Clip stray("Stray", 0.0, 4.0);
        zs.editTrack(0).clips.push_back(stray);
        const uint64_t r0 = zs.revision(), t0 = zs.trackRevision(0), s0 = zs.structureRevision();
        const bool fixed = zs.normalizeIds();
        c.check(fixed && zs.revision() > r0 && zs.trackRevision(0) > t0 && zs.structureRevision() == s0 &&
                    zs.project().tracks[0].clips.back().id != daw::kNoId && distinctNonZero(clipIds(zs.project())),
                "Ids: a stray clip gets an id; revisions move, structure does not");

        // replace with zero ids: the new ids are all above every id the store gave before
        const daw::ObjectId before = highest(zs.project());
        zs.replace(shape(0, 0, 0, 0, 0));
        bool above = true;
        for (daw::ObjectId id : trackIds(zs.project()))
            above = above && id > before;
        for (daw::ObjectId id : clipIds(zs.project()))
            above = above && id > before;
        c.check(above && distinctNonZero(trackIds(zs.project())) && distinctNonZero(clipIds(zs.project())),
                "Ids: replace gives new ids above every id given before");

        // Ids are ignored by sameContent and sameSaved, so an id-only difference is not a change
        daw::Project sa = daw::demoProject();
        daw::Project sb = sa;
        sb.tracks[0].id = 77;
        sb.tracks[0].clips[0].id = 88;
        sb.tracks[1].id = 0;
        c.check(daw::sameContent(sa, sb) && daw::sameSaved(sa, sb), "Ids: sameContent and sameSaved ignore ids");
        daw::History ih;
        ih.reset(sa);
        const bool idStep = ih.update(sb, true, false, 1.0);
        c.check(!idStep && !ih.canUndo(), "Ids: an id-only difference is not an undo step");

        // Through a file: ids saved and read back unchanged, a track's table starting with its id
        daw::Project idp = shape(7, 3, 11, 12, 13);
        const std::string idText = daw::writeProject(idp);
        daw::Project idBack;
        std::string idError;
        const bool idOk = daw::readProject(idText, idBack, idError);
        c.check(idOk && trackIds(idBack) == trackIds(idp) && clipIds(idBack) == clipIds(idp) && daw::writeProject(idBack) == idText,
                "Ids: saved ids read back unchanged", idError);
        const size_t tablePos = idText.find("[[tracks]]\n");
        c.check(tablePos != std::string::npos && idText.compare(tablePos + 11, 5, "id = ") == 0,
                "Ids: a track's table starts with its id");

        // A file with no id keys reads with unique non-zero ids
        const std::string bare = "format = 3\nname = \"Bare ids\"\n[[tracks]]\nname = \"One\"\n[[tracks.clips]]\nname = \"a\"\n"
                                 "[[tracks.clips]]\nname = \"b\"\n[[tracks]]\nname = \"Two\"\n[[tracks.clips]]\nname = \"c\"\n";
        daw::Project bareP;
        std::string bareError;
        const bool bareIds = daw::readProject(bare, bareP, bareError);
        c.check(bareIds && bareP.tracks.size() == 2 && distinctNonZero(trackIds(bareP)) && distinctNonZero(clipIds(bareP)),
                "Ids: a file without ids reads with unique non-zero ids", bareError);

        // Invalid ids refuse the file, with a reason
        const std::string negTrack = refusal("format = 3\nname = \"Bad\"\n[[tracks]]\nid = -1\nname = \"A\"\n");
        const std::string textTrack = refusal("format = 3\n[[tracks]]\nid = \"x\"\nname = \"A\"\n");
        const std::string bigTrack = refusal("format = 3\n[[tracks]]\nid = 4294967296\nname = \"A\"\n");
        const std::string negClip = refusal("format = 3\n[[tracks]]\nname = \"A\"\n[[tracks.clips]]\nid = -1\nname = \"c\"\n");
        c.check(!negTrack.empty(), "Ids: a negative track id refuses the file", negTrack);
        c.check(!textTrack.empty(), "Ids: a text track id refuses the file", textTrack);
        c.check(!bigTrack.empty(), "Ids: a track id above 4294967295 refuses the file", bigTrack);
        c.check(!negClip.empty(), "Ids: a negative clip id refuses the file", negClip);

        // The largest id is allowed
        daw::Project maxP;
        const bool maxOk = daw::readProject("format = 3\n[[tracks]]\nid = 4294967295\nname = \"A\"\n", maxP, bareError);
        c.check(maxOk && maxP.tracks.size() == 1 && maxP.tracks[0].id == 4294967295u, "Ids: the largest id is read", bareError);
    }

    // Automation: lanes on a track and on its devices read back exactly; a bad lane is refused with a
    // reason; out-of-range points are clamped and sorted; editing one is an undo step of its own, put
    // back by moving only its track
    {
        daw::Project ap;
        daw::Track t;
        t.name = "Automated";
        t.kind = daw::TrackKind::Instrument;
        t.instrument = audio::SynthDevice::setting();
        t.instrument.automation.push_back({3, {{0.0, 0.25f}, {2.5, 1.0f}}});
        t.effects.push_back(audio::effectSetting(audio::EffectType::Gain));
        t.effects[0].automation.push_back({0, {{1.0, 0.5f}}});
        t.effects[0].automation.push_back({1, {}}); // shown, but plays nothing
        t.automation.push_back({audio::kVolumeParam, {{0.0, 0.0f}, {4.0, 0.79f}, {8.0, 0.5f}}});
        t.automation.push_back({audio::kPanParam, {{2.0, 0.1f}}});
        ap.tracks.push_back(t);
        const std::string text = daw::writeProject(ap);
        daw::Project back;
        std::string why;
        const bool ok = daw::readProject(text, back, why);
        const bool same = ok && back.tracks.size() == 1 && back.tracks[0].automation == ap.tracks[0].automation &&
                          back.tracks[0].instrument.automation == ap.tracks[0].instrument.automation &&
                          back.tracks[0].effects.size() == 1 && back.tracks[0].effects[0].automation == ap.tracks[0].effects[0].automation;
        // Read again, the text is the same (the first read gives the tracks their ids)
        daw::Project again;
        const bool againOk = ok && daw::readProject(daw::writeProject(back), again, why);
        c.check(same && daw::sameContent(back, ap) && againOk && daw::writeProject(again) == daw::writeProject(back),
                "Automation: a track's and its devices' lanes read back exactly (an empty lane too)", why);

        auto refused = [](const std::string &file) {
            daw::Project p;
            std::string error;
            return daw::readProject(file, p, error) ? std::string() : error;
        };
        const std::string badTrack = refused("format = 3\n[[tracks]]\nname = \"A\"\nautomation = [ { param = \"loud\", points = [] } ]\n");
        const std::string badDevice = refused(
            "format = 3\n[[tracks]]\nname = \"A\"\n[[tracks.effects]]\nkind = \"effect\"\nid = \"gain\"\nparams = []\n"
            "automation = [ { param = \"volume\", points = [] } ]\n");
        c.check(badTrack.find("volume") != std::string::npos && !badDevice.empty(),
                "Automation: a lane for something that isn't there is refused with a reason", badTrack + " / " + badDevice);
        daw::Project wild;
        const bool wildOk = daw::readProject(
            "format = 3\n[[tracks]]\nname = \"A\"\nautomation = [ { param = \"pan\", points = [ { beat = 4, value = 3 }, "
            "{ beat = -2, value = -1 } ] }, { param = \"pan\", points = [] } ]\n",
            wild, why);
        const auto &wildLanes = wildOk && !wild.tracks.empty() ? wild.tracks[0].automation : std::vector<audio::AutomationLane>{};
        c.check(wildLanes.size() == 1 && wildLanes[0].points.size() == 2 && wildLanes[0].points[0].beat == 0.0 &&
                    wildLanes[0].points[0].value == 0.0f && wildLanes[0].points[1].value == 1.0f,
                "Automation: points are clamped and sorted, a second lane for one parameter dropped", why);

        // Undo: moving a point is "Edit automation" and only that track's content comes back
        daw::ProjectStore store(ap);
        store.edit().tracks.push_back(daw::Track{});
        daw::History history;
        history.reset(store.project());
        store.editTrack(0).automation[0].points[1].value = 0.4f;
        const bool recorded = history.update(store, true, false, 1.0);
        const uint64_t other = store.trackRevision(1), structure = store.structureRevision();
        daw::HistoryRestore plan;
        const bool undone = history.undo(store, 2.0, plan);
        store.restore(std::move(plan));
        c.check(recorded && history.redoLabel() == "Edit automation" && undone &&
                    store.project().tracks[0].automation[0].points[1].value == 0.79f && store.trackRevision(1) == other &&
                    store.structureRevision() == structure,
                "Automation: editing a lane is an undo step, put back without touching other tracks", history.redoLabel());
        store.editDevice(daw::DevicePlace::effect(false, 0, 0))->automation[0].points[0].value = 0.9f;
        history.update(store, true, false, 3.0);
        c.check(history.undoLabel() == "Edit automation", "Automation: a device's lane edit is named too", history.undoLabel());
    }

    // Recording automation (a latch pass): the lane's own value up to the touch, the written values
    // replacing what was there, its own value back after, thinned; a held value adds no points; the
    // playhead going back starts another stretch; and through AutomationWriter, only an armed track
    // while playing, never the project's own values, the whole pass one undo step
    {
        using audio::AutomationWrite;
        auto near = [](double a, double b) { return std::fabs(a - b) < 1e-4; };
        const double gap = AutomationWrite::kGap;

        audio::AutomationLane lane{audio::kVolumeParam, {{0.0, 0.2f}, {3.0, 0.1f}, {8.0, 0.8f}}};
        const audio::AutomationLane before = lane;
        AutomationWrite w = audio::beginAutomationWrite(lane, 2.0, 0.9f, false, 0.004f);
        const float ownAtTouch = before.positionAt(2.0 - gap, false);
        c.check(near(w.from, 2.0 - gap) && lane.points.size() == 5 && near(lane.positionAt(2.0 - gap, false), ownAtTouch) &&
                    lane.positionAt(2.0, false) == 0.9f,
                "Automation recording: a touch holds the lane's own value until it, then the value written");
        for (double beat = 2.25; beat <= 3.5; beat += 0.25)
            audio::writeAutomation(lane, w, beat, 0.9f);
        size_t inPass = 0;
        for (const auto &p : lane.points)
            inPass += p.beat >= w.from && p.beat <= w.to;
        c.check(inPass == 3 && lane.positionAt(3.0, false) == 0.9f && near(w.to, 3.5),
                "Automation recording: a held value moves its last point on (the point at beat 3 replaced)",
                std::to_string(inPass) + " points in the pass");
        audio::writeAutomation(lane, w, 4.0, 0.5f);
        audio::writeAutomation(lane, w, 5.0, 0.5f);
        audio::finishAutomationWrite(lane, w);
        const float ownAfter = before.positionAt(5.0 + gap, false);
        bool untouched = lane.points.front() == before.points.front() && lane.points.back() == before.points.back();
        bool oldGone = std::none_of(lane.points.begin(), lane.points.end(), [](const audio::AutomationPoint &p) { return p.beat == 3.0; });
        c.check(untouched && oldGone && near(lane.positionAt(4.5, false), 0.5) && near(lane.positionAt(5.0 + gap, false), ownAfter) &&
                    near(lane.positionAt(6.5, false), before.positionAt(6.5, false)),
                "Automation recording: finishing puts the lane's own value back after the pass, the points around it kept");

        // A ramp written frame by frame is thinned to what the line needs
        audio::AutomationLane ramp{0, {}};
        AutomationWrite rw = audio::beginAutomationWrite(ramp, 0.0, 0.0f, false, 0.004f);
        for (int i = 1; i <= 64; ++i)
            audio::writeAutomation(ramp, rw, i / 16.0, static_cast<float>(i) / 64.0f);
        const size_t written = ramp.points.size();
        audio::finishAutomationWrite(ramp, rw);
        c.check(written == 65 && ramp.points.size() == 2 && ramp.points.back().beat == 4.0 && ramp.points.back().value == 1.0f,
                "Automation recording: a written ramp is thinned to its ends (an empty lane gets no return point)",
                std::to_string(written) + " -> " + std::to_string(ramp.points.size()));

        // A loop: the playhead goes back, so the stretch so far ends and another begins
        audio::AutomationLane looped{0, {{0.0, 0.5f}}};
        AutomationWrite lw = audio::beginAutomationWrite(looped, 4.0, 1.0f, false, 0.004f);
        audio::writeAutomation(looped, lw, 6.0, 1.0f);
        audio::writeAutomation(looped, lw, 1.0, 0.0f); // back to the loop's start
        audio::writeAutomation(looped, lw, 2.0, 0.0f);
        audio::finishAutomationWrite(looped, lw);
        c.check(near(looped.positionAt(1.5, false), 0.0) && near(looped.positionAt(5.0, false), 1.0) &&
                    near(looped.positionAt(0.5, false), 0.5) && near(lw.from, 1.0 - gap),
                "Automation recording: going back keeps the first stretch and writes the second");

        // A stepped parameter holds each value
        audio::AutomationLane steps{0, {}};
        AutomationWrite sw = audio::beginAutomationWrite(steps, 0.0, 0.0f, true, 0.004f);
        audio::writeAutomation(steps, sw, 1.0, 0.0f);
        audio::writeAutomation(steps, sw, 2.0, 1.0f);
        audio::writeAutomation(steps, sw, 3.0, 1.0f);
        audio::finishAutomationWrite(steps, sw);
        c.check(steps.positionAt(1.9, true) == 0.0f && steps.positionAt(2.0, true) == 1.0f && steps.points.size() == 3,
                "Automation recording: a stepped lane steps where the value changed, repeats between dropped",
                std::to_string(steps.points.size()));

        // AutomationWriter: a track's volume and a device's knob, one undo step
        daw::Project wp;
        daw::Track wt;
        wt.name = "Written";
        wt.effects.push_back(audio::effectSetting(audio::EffectType::Gain));
        wp.tracks.push_back(wt);
        wp.masterEffects.push_back(audio::effectSetting(audio::EffectType::Gain));
        daw::ProjectStore wstore(wp);
        double playhead = 1.0;
        bool playing = false;
        daw::AutomationWriter writer(wstore, [&] { return playhead; }, [&] { return playing; });
        daw::History wh;
        wh.reset(wstore.project());
        const float gainBefore = wstore.project().tracks[0].gain;
        const daw::DevicePlace gainPlace = daw::DevicePlace::effect(false, 0, 0);
        const daw::AutomationPlace devicePlace{false, gainPlace};
        const audio::ParamCurve levelCurve = daw::automationCurve(audio::effectParam(audio::effectInfo(audio::EffectType::Gain).params[0], 0));
        const bool idle = writer.touchTrack(0, audio::kVolumeParam, 0.5);
        playing = true;
        const bool unarmed = writer.touchTrack(0, audio::kVolumeParam, 0.5);
        writer.setArmed(0, true);
        const bool master = writer.touch(0, daw::AutomationPlace{false, daw::DevicePlace::effect(true, -1, 0)}, 0, levelCurve, 0.0);
        c.check(!idle && !unarmed && !master && !writer.writing() && writer.armed(0),
                "Automation recording: nothing is written while stopped, unarmed, or for a master whose writing is off");
        int steps0 = 0;
        double now = 0.0;
        auto frame = [&] {
            writer.update();
            now += 0.1;
            steps0 += wh.update(wstore, !writer.writing(), false, now) ? 1 : 0;
        };
        const bool took = writer.touchTrack(0, audio::kVolumeParam, 0.5);
        frame();
        for (int i = 0; i < 8; ++i) {
            playhead += 0.25;
            if (i == 2)
                writer.touch(0, devicePlace, 0, levelCurve, -6.0);
            if (i == 4)
                writer.touchTrack(0, audio::kVolumeParam, 1.0);
            frame();
        }
        const bool wasWriting = writer.writing();
        playing = false;
        frame();
        const daw::Track &after = wstore.project().tracks[0];
        const audio::AutomationLane *vol = audio::findAutomation(after.automation, audio::kVolumeParam);
        const audio::AutomationLane *lvl = audio::findAutomation(after.effects[0].automation, 0);
        const audio::ParamCurve volCurve = daw::automationCurve(audio::kVolumeParam);
        c.check(took && wasWriting && !writer.writing() && vol && lvl && after.gain == gainBefore &&
                    after.effects[0].values == wp.tracks[0].effects[0].values &&
                    near(volCurve.value(vol->positionAt(1.25, false)), 0.5) && near(volCurve.value(vol->positionAt(3.0, false)), 1.0) &&
                    near(levelCurve.value(lvl->positionAt(3.0, false)), -6.0),
                "Automation recording: the fader and a knob write their lanes from the touch to the stop, the own values untouched");
        c.check(steps0 == 1 && wh.undoLabel() == "Edit automation", "Automation recording: the whole pass is one undo step",
                std::to_string(steps0) + " steps, " + wh.undoLabel());
        // Moving a control now that playback has stopped sets the project's value as before
        c.check(!writer.touchTrack(0, audio::kVolumeParam, 0.25), "Automation recording: stopped, a control sets its own value again");

        // Touch mode: written only while the control is held. A volume lane flat at the fader's 0.25 position;
        // held from beat 2 to beat 3 at a higher value, then let go while the song plays on: the lane has the
        // written value up to beat 3, glides back to its own by 3.25, and nothing after is written
        {
            daw::Project tp;
            daw::Track tt;
            tt.name = "Touched";
            tt.automation.push_back({audio::kVolumeParam, {{0.0, 0.25f}, {8.0, 0.25f}}});
            tp.tracks.push_back(tt);
            daw::ProjectStore ts(tp);
            double head = 2.0;
            bool rolling = true;
            daw::AutomationWriter touch(ts, [&] { return head; }, [&] { return rolling; });
            touch.setMode(daw::AutomationWriter::Mode::Touch);
            touch.setArmed(0, true);
            const audio::ParamCurve curve = daw::automationCurve(audio::kVolumeParam);
            const float held = 0.75f; // a position: the gain written is curve.value(held)
            touch.touchTrack(0, audio::kVolumeParam, curve.value(held));
            for (int i = 0; i < 4; ++i) {
                head += 0.25;
                touch.update(true);
            }
            const bool writingHeld = touch.writing();
            touch.update(false); // let go at beat 3
            const bool writingAfter = touch.writing();
            head = 5.0;
            touch.update(false); // later frames write nothing
            const audio::AutomationLane *lane = audio::findAutomation(ts.project().tracks[0].automation, audio::kVolumeParam);
            const float at25 = lane ? lane->positionAt(2.5, false) : -1.0f, at3 = lane ? lane->positionAt(3.0, false) : -1.0f;
            const float at3125 = lane ? lane->positionAt(3.125, false) : -1.0f, at4 = lane ? lane->positionAt(4.0, false) : -1.0f;
            c.check(writingHeld && !writingAfter && std::fabs(at25 - held) < 1e-3f && std::fabs(at3 - held) < 1e-3f &&
                        at3125 < held - 0.1f && at3125 > 0.3f && std::fabs(at4 - 0.25f) < 1e-3f,
                    "Automation recording: touch mode writes while held, then glides back to the lane over a quarter beat",
                    std::to_string(at25) + " " + std::to_string(at3) + " " + std::to_string(at3125) + " " + std::to_string(at4));
            // Latch mode ignores the mouse: letting go keeps writing until playback stops
            touch.setMode(daw::AutomationWriter::Mode::Latch);
            touch.touchTrack(0, audio::kVolumeParam, curve.value(held));
            head = 6.0;
            touch.update(false);
            const bool latched = touch.writing();
            rolling = false;
            touch.update(false);
            c.check(latched && !touch.writing(), "Automation recording: latch mode keeps writing when the mouse is let go");
        }

        // The master: its places, its lane rows, and a pass writing its volume and an effect's knob
        {
            daw::Project mp = wp;
            mp.masterVolume = 0.8f;
            mp.masterAutomation.push_back({audio::kVolumeParam, {{0.0, 0.5f}}});
            mp.masterEffects[0].automation.push_back({0, {{1.0, 0.25f}}});
            mp.masterEffects.push_back(audio::effectSetting(audio::EffectType::Gain));
            mp.masterEffects[1].automation.push_back({0, {}});
            daw::ProjectStore ms(mp);
            const auto *volumeLanes = daw::lanesAt(ms.project(), 7, daw::AutomationPlace::masterVolume());
            const auto *effectLanes = daw::lanesAt(ms.project(), 7, daw::AutomationPlace::masterEffect(1));
            const auto *gone = daw::lanesAt(ms.project(), 7, daw::AutomationPlace::masterEffect(5));
            c.check(volumeLanes == &ms.project().masterAutomation && effectLanes == &ms.project().masterEffects[1].automation && !gone &&
                        !(daw::AutomationPlace::masterVolume() == daw::AutomationPlace{}) &&
                        !(daw::AutomationPlace::masterEffect(0) == daw::AutomationPlace::masterEffect(1)),
                    "Master automation: lanesAt finds the master's volume and effects, whatever the track");
            const uint64_t before = ms.masterRevision(), trackBefore = ms.trackRevision(0);
            auto *edited = daw::editLanesAt(ms, 7, daw::AutomationPlace::masterVolume());
            const uint64_t afterVolume = ms.masterRevision();
            auto *editedEffect = daw::editLanesAt(ms, 7, daw::AutomationPlace::masterEffect(0));
            c.check(edited == &ms.project().masterAutomation && editedEffect == &ms.project().masterEffects[0].automation &&
                        afterVolume != before && ms.masterRevision() != afterVolume && ms.trackRevision(0) == trackBefore,
                    "Master automation: editLanesAt edits through the master's revision alone");
            const std::vector<daw::AutomationRow> rows = daw::masterAutomationRows(ms.project());
            c.check(rows.size() == 3 && rows[0].place == daw::AutomationPlace::masterVolume() && rows[0].param == audio::kVolumeParam &&
                        rows[1].place == daw::AutomationPlace::masterEffect(0) && rows[2].place == daw::AutomationPlace::masterEffect(1),
                    "Master automation: the lane rows are the volume, then each effect's", std::to_string(rows.size()));

            // Writing: nothing until the master is armed (an armed track doesn't arm it), then one pass, one step
            double mhead = 2.0;
            bool mplaying = true;
            daw::AutomationWriter mw(ms, [&] { return mhead; }, [&] { return mplaying; });
            mw.setArmed(0, true);
            const bool unarmedMaster = mw.touchMaster(1.0);
            mw.setMasterArmed(true);
            daw::History mh;
            mh.reset(ms.project());
            ms.editMasterAutomation().clear();
            ms.editMasterEffects()[0].automation.clear();
            mh.reset(ms.project());
            int mSteps = 0;
            double mnow = 0.0;
            auto mframe = [&] {
                mw.update();
                mnow += 0.1;
                mSteps += mh.update(ms, !mw.writing(), false, mnow) ? 1 : 0;
            };
            const float volumeBefore = ms.project().masterVolume;
            const audio::ParamCurve gainCurve = daw::automationCurve(audio::effectParam(audio::effectInfo(audio::EffectType::Gain).params[0], 0));
            const bool wroteVolume = mw.touchMaster(0.5);
            mframe();
            for (int i = 0; i < 8; ++i) {
                mhead += 0.25;
                if (i == 2)
                    mw.touch(0, daw::AutomationPlace{false, daw::DevicePlace::effect(true, -1, 0)}, 0, gainCurve, -6.0);
                if (i == 4)
                    mw.touchMaster(1.5);
                mframe();
            }
            const bool writing = mw.writing();
            mplaying = false;
            mframe();
            const audio::AutomationLane *mvol = audio::findAutomation(ms.project().masterAutomation, audio::kVolumeParam);
            const audio::AutomationLane *mlvl = audio::findAutomation(ms.project().masterEffects[0].automation, 0);
            c.check(!unarmedMaster && wroteVolume && writing && !mw.writing() && mvol && mlvl && ms.project().masterVolume == volumeBefore &&
                        near(volCurve.value(mvol->positionAt(2.0, false)), 0.5) && near(volCurve.value(mvol->positionAt(4.0, false)), 1.5) &&
                        near(gainCurve.value(mlvl->positionAt(4.0, false)), -6.0) && daw::masterVolumeAt(ms.project(), 2.0) < 0.6,
                    "Master automation: the volume and an effect's knob write their lanes from the touch to the stop");
            c.check(mSteps == 1 && mh.undoLabel() == "Edit automation",
                    "Master automation: the whole recording pass is one undo step", std::to_string(mSteps) + " steps, " + mh.undoLabel());
            // Disarming finishes a pass under way
            mplaying = true;
            mw.touchMaster(1.0);
            const bool underway = mw.writing();
            mw.setMasterArmed(false);
            c.check(underway && !mw.writing() && !mw.touchMaster(1.0), "Master automation: disarming finishes the master's pass");
            // The master strip shows masterVolumeAt at the writer's playhead: it follows the playhead
            // along the written lane, and a lane with no points shows the project's own volume
            const audio::AutomationLane *shown = audio::findAutomation(ms.project().masterAutomation, audio::kVolumeParam);
            mhead = 2.0;
            const double at2 = daw::masterVolumeAt(ms.project(), mw.playhead());
            mhead = 4.0;
            const double at4 = daw::masterVolumeAt(ms.project(), mw.playhead());
            c.check(shown && near(at2, volCurve.value(shown->positionAt(2.0, false))) && near(at4, volCurve.value(shown->positionAt(4.0, false))) &&
                        at2 != at4,
                    "Master automation: the strip's fader value follows the playhead along the lane", std::to_string(at2) + " then " + std::to_string(at4));
            ms.editMasterAutomation().clear();
            c.check(near(daw::masterVolumeAt(ms.project(), mw.playhead()), ms.project().masterVolume),
                    "Master automation: with no lane the strip shows the master volume");
        }
    }

    // The audio bridge keeps processors across structural edits (its own checks)
    c.failures += runBridgeChecks();
    c.failures += runRoutingChecks();
    c.failures += runTempoChecks();
    c.failures += runTakesChecks();
    c.failures += runWarpModelChecks();
    c.failures += runMarkersChecks();
    c.failures += runMidiImportChecks();
    c.failures += runFreezeProjectChecks();

    return c.finish("project");
}

}
