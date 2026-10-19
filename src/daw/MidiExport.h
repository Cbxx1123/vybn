#pragma once

#include <string>
#include <vector>

#include "Project.h"

namespace daw {

// What a MIDI export holds: the instrument tracks (project indices; empty for every instrument
// track with notes or controller points in range), a range in beats (toBeat -1 for the end of the
// last clip), and whether the first track plays on MIDI's drum channel (9)
struct MidiExportOptions {
    std::vector<int> tracks;
    double fromBeat = 0.0;
    double toBeat = -1.0;
    bool drumChannel = false;
};

// Writes a Standard MIDI File (type 1, 480 ticks a beat): a conductor track with the project's name,
// tempo and time signature, then one track per exported instrument track. Notes are written from
// every clip (muted ones left out, cut at the clip's end and at the range's end), controller lanes
// as controller changes (ramps sampled every 1/32 beat, only where the value changes) and the pitch
// bend as 14-bit messages. Time 0 of the file is `fromBeat`. Written beside `path` and renamed, so a
// failed write leaves nothing behind; returns false with a reason in `error`.
bool writeMidiFile(const Project &project, const MidiExportOptions &options, const std::string &path, std::string &error);

}
