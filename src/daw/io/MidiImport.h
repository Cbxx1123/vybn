#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "daw/model/Project.h"

// Standard MIDI Files in (docs/design/markers-and-import-plan.md): the reader, and the file put into
// a project. Positions are beats (quarter notes: ticks over the file's ticks per quarter), so the
// tempo never moves them; the file's tempo and signatures are kept apart, for the project to take
// or not.

namespace daw {

// A part of the file: a track with notes or controllers (a type 0 file's single track is split by
// channel), its notes and curves in beats from the file's start
struct MidiPart {
    std::string name;
    int channel = 0;  // the channel its first note or controller is on (0-15; 9 is drums)
    int program = -1; // its first program change, or -1
    std::vector<Note> notes;
    std::vector<ControlLane> controls;
    double end() const; // where its last note ends or last point is
};

struct MidiFile {
    int format = 1;
    int ticksPerQuarter = 480;
    std::string name; // the first track's name in a type 1 file (the conductor's), else empty
    std::vector<audio::TempoPoint> tempo; // from beat 0 (held), sorted; empty: none in the file
    struct Meter {
        double beat = 0.0;
        int numerator = 4;
        int denominator = 4;
    };
    std::vector<Meter> signatures; // sorted; empty: none in the file
    std::vector<MidiPart> parts;
    size_t noteCount() const;
};

// Reads a file's bytes. Refused, with a reason: anything not a type 0 or 1 MIDI file, SMPTE time,
// and a file that runs short or claims more than it holds (nothing is read past the end).
bool readMidi(const std::vector<uint8_t> &bytes, MidiFile &out, std::string &error);
bool readMidiFile(const std::string &path, MidiFile &out, std::string &error);

// The file's parts as clips from beat `at`, each to the bar line at or after its end: the first on
// `track` when that is an instrument track (-1: none), the others on new instrument tracks playing
// `instrument`, after it (or at the end), named after their parts (`fallback` and a number when a
// part has no name). The timeline grows to hold them. Returns the first track used, or -1 when the
// file has no notes or controllers.
int importMidi(Project &project, const MidiFile &file, double at, int track, const audio::DeviceSetting &instrument,
               const std::string &fallback);

// The project's tempo map with the file's laid over it from beat `at` on: the file's tempo points
// from there (the project's after it go), its signatures from the bar `at` is in (likewise). The
// project's own when the file has none.
audio::TempoMap fileTiming(const audio::TempoMap &project, const MidiFile &file, double at);

}
