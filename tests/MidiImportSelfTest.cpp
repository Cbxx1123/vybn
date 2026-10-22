#include "MidiImportSelfTest.h"
#include "Harness.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "audio/Synth.h"
#include "daw/io/MidiExport.h"
#include "daw/io/MidiImport.h"

namespace tests {

namespace {

bool near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance;
}

// A file's bytes: the header, then each track's events in a chunk
std::vector<uint8_t> smf(int format, int ticks, const std::vector<std::vector<uint8_t>> &tracks)
{
    std::vector<uint8_t> out = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, static_cast<uint8_t>(format), 0, static_cast<uint8_t>(tracks.size()),
                                static_cast<uint8_t>(ticks >> 8), static_cast<uint8_t>(ticks)};
    for (const auto &events : tracks) {
        const uint32_t n = static_cast<uint32_t>(events.size());
        out.insert(out.end(), {'M', 'T', 'r', 'k', static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16), static_cast<uint8_t>(n >> 8),
                               static_cast<uint8_t>(n)});
        out.insert(out.end(), events.begin(), events.end());
    }
    return out;
}

}

int runMidiImportChecks()
{
    Checker c;
    std::string error;

    // A file the exporter wrote comes back: notes, a controller, the bend, the tempo and signatures
    {
        daw::Project p;
        p.name = "Round trip";
        p.timing.setPoints({{0.0, 100.0, false}, {8.0, 140.0, false}});
        p.timing.setSignatures({{0, 4, 4}, {3, 7, 8}});
        daw::Track keys;
        keys.name = "Keys";
        keys.kind = daw::TrackKind::Instrument;
        daw::Clip clip("Riff", 0.0, 16.0, {{0.0, 1.0, 60, 100.0f / 127.0f}, {1.5, 0.25, 64, 64.0f / 127.0f}, {12.0, 2.0, 67, 1.0f}});
        clip.controls.push_back({1, {{0.0, 0.0f}, {4.0, 1.0f}}});
        clip.controls.push_back({daw::kPitchBend, {{2.0, 0.75f}}});
        keys.clips.push_back(clip);
        p.tracks.push_back(keys);
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path path = fs::temp_directory_path(ec) / "vybn-import-selftest.mid";
        const bool wrote = daw::writeMidiFile(p, {}, path.u8string(), error);
        daw::MidiFile file;
        const bool read = wrote && daw::readMidiFile(path.u8string(), file, error);
        fs::remove(path, ec);
        bool notes = read && file.parts.size() == 1 && file.parts[0].name == "Keys" && file.parts[0].notes.size() == 3;
        if (notes) {
            const auto &n = file.parts[0].notes;
            notes = near(n[1].start, 1.5) && near(n[1].length, 0.25) && n[1].key == 64 && near(n[1].velocity, 64.0 / 127.0) &&
                    near(n[2].start, 12.0) && near(n[2].length, 2.0) && n[2].velocity == 1.0f;
        }
        const bool curves = read && file.parts[0].controls.size() == 2;
        const bool tempo = read && file.tempo.size() == 2 && near(file.tempo[0].bpm, 100.0, 1e-3) && near(file.tempo[1].beat, 8.0) &&
                           near(file.tempo[1].bpm, 140.0, 1e-3);
        const bool metre = read && file.signatures.size() == 2 && near(file.signatures[1].beat, 12.0) && file.signatures[1].numerator == 7 &&
                           file.signatures[1].denominator == 8 && file.name == "Round trip";
        c.check(notes && curves && tempo && metre, "MIDI import: a file the exporter wrote reads back", error);
    }

    // Hand-made: a type 0 file at 96 ticks a beat, two channels, running status, velocity 0 as note
    // off, two overlapping notes on one key (paired first in, first out), a track name
    {
        const std::vector<uint8_t> events = {
            0x00, 0xFF, 0x03, 0x04, 'B', 'a', 'n', 'd',  // name
            0x00, 0x90, 60, 100,                         // ch1 C4 on at 0
            0x30, 60, 90,                                // (running) C4 on again at 48
            0x30, 60, 0,                                 // C4 off (velocity 0) at 96: the first
            0x00, 0x99, 36, 120,                         // ch10 kick on at 96
            0x18, 0x89, 36, 0,                           // kick off at 120
            0x18, 0x80, 60, 0,                           // C4 off at 144: the second
            0x00, 0xFF, 0x2F, 0x00};
        daw::MidiFile file;
        const bool read = daw::readMidi(smf(0, 96, {events}), file, error);
        bool ok = read && file.format == 0 && file.parts.size() == 2;
        if (ok) {
            const daw::MidiPart &melody = file.parts[0];
            const daw::MidiPart &drums = file.parts[1];
            ok = melody.notes.size() == 2 && near(melody.notes[0].start, 0.0) && near(melody.notes[0].length, 1.0) &&
                 near(melody.notes[1].start, 0.5) && near(melody.notes[1].length, 1.0) && drums.channel == 9 &&
                 drums.name == "Band Drums" && drums.notes.size() == 1 && near(drums.notes[0].length, 0.25);
        }
        c.check(ok, "MIDI import: running status, velocity-0 offs, a type 0 file split by channel", error);
    }

    // Refused, with a reason, never read past the end
    {
        daw::MidiFile file;
        std::vector<uint8_t> riff = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        std::vector<uint8_t> smpte = smf(1, 0xE728, {{0x00, 0xFF, 0x2F, 0x00}});
        std::vector<uint8_t> type2 = smf(2, 96, {{0x00, 0xFF, 0x2F, 0x00}});
        std::vector<uint8_t> shortTrack = smf(0, 96, {{0x00, 0x90, 60}}); // a note on cut short
        std::vector<uint8_t> lying = smf(0, 96, {{0x00, 0xFF, 0x2F, 0x00}});
        lying[21] = 0x7F; // the track claims far more than the file holds
        std::string a, b, d, e, f;
        const bool refused = !daw::readMidi(riff, file, a) && !daw::readMidi(smpte, file, b) && !daw::readMidi(type2, file, d) &&
                             !daw::readMidi(shortTrack, file, e) && !daw::readMidi(lying, file, f);
        c.check(refused && !a.empty() && b.find("SMPTE") != std::string::npos && d.find("type 2") != std::string::npos && !e.empty() && !f.empty(),
                "MIDI import: files that aren't, or that lie, are refused", e + " / " + f);
    }

    // Into a project: the first part on the instrument track aimed at, the other on a new track after it
    {
        daw::MidiFile file;
        file.parts.resize(2);
        file.parts[0].name = "Lead";
        file.parts[0].notes = {{0.0, 1.0, 72, 0.8f}, {5.0, 1.0, 74, 0.8f}};
        file.parts[1].notes = {{0.0, 2.0, 36, 0.8f}};
        daw::Project p;
        p.bars = 4;
        daw::Track lead;
        lead.kind = daw::TrackKind::Instrument;
        p.tracks = {lead, daw::Track{}};
        const int first = daw::importMidi(p, file, 12.0, 0, audio::SynthDevice::setting(), "Song");
        const bool placed = first == 0 && p.tracks.size() == 3 && p.tracks[0].clips.size() == 1 && p.tracks[0].clips[0].start == 12.0 &&
                            p.tracks[0].clips[0].length == 8.0 && p.tracks[1].name == "Song 2" &&
                            p.tracks[1].kind == daw::TrackKind::Instrument && p.tracks[1].instrument.kind == audio::DeviceKind::Synth &&
                            p.tracks[1].clips[0].notes.size() == 1 && p.bars >= 6;
        c.check(placed, "MIDI import: parts land on the track aimed at, then new tracks after it",
                std::to_string(p.tracks.size()) + " tracks, " + std::to_string(p.bars) + " bars");
    }

    // The file's tempo map laid over the project's from where it lands
    {
        daw::MidiFile file;
        file.tempo = {{0.0, 90.0, false}, {6.0, 80.0, false}};
        file.signatures = {{0.0, 3, 4}, {6.0, 6, 8}};
        audio::TempoMap project(120.0);
        project.setPoints({{0.0, 120.0, false}, {16.0, 100.0, false}});
        const audio::TempoMap laid = daw::fileTiming(project, file, 8.0);
        const auto &points = laid.points();
        const auto &signatures = laid.signatures();
        c.check(points.size() == 3 && points[1].beat == 8.0 && points[1].bpm == 90.0 && points[2].beat == 14.0 && signatures.size() == 3 &&
                    signatures[1].bar == 2 && signatures[1].numerator == 3 && signatures[2].bar == 4 && signatures[2].denominator == 8 &&
                    daw::fileTiming(project, daw::MidiFile(), 8.0) == project,
                "MIDI import: the file's tempo and signatures laid over the project's",
                std::to_string(points.size()) + " points, " + std::to_string(signatures.size()) + " signatures");
    }
    return c.failures;
}

}
