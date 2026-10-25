#include "daw/model/Transcription.h"

#include <algorithm>
#include <cmath>

#include "daw/model/Warp.h"

namespace daw {

const char *conversionName(Conversion kind)
{
    switch (kind) {
    case Conversion::Drums: return "drums";
    case Conversion::Melody: return "melody";
    case Conversion::Harmony: return "harmony";
    }
    return "notes";
}

std::vector<double> clipBeatBoundaries(const Project &project, const Clip &clip)
{
    std::vector<double> out;
    const double end = clip.start + clip.length;
    out.push_back(clipSourceSeconds(clip, project.timing, clip.start));
    for (double beat = std::floor(clip.start) + 1.0; beat < end - 1e-9; beat += 1.0)
        out.push_back(clipSourceSeconds(clip, project.timing, beat));
    out.push_back(clipSourceSeconds(clip, project.timing, end));
    return out;
}

Clip transcribedClip(const Project &project, const Clip &audio, const std::vector<audio::TranscribedNote> &notes, Conversion kind)
{
    Clip clip(audio.name + " " + conversionName(kind), audio.start, audio.length);
    const audio::TempoMap &timing = project.timing;
    for (const audio::TranscribedNote &found : notes) {
        // Where it falls in the clip, in beats from the clip's start
        const double beat = clipBeatAt(audio, timing, found.start);
        const double at = beat - audio.start;
        if (at < -1e-9 || at >= audio.length)
            continue;
        Note note;
        note.start = std::max(0.0, at);
        note.key = found.key;
        note.velocity = found.velocity;
        if (kind == Conversion::Drums)
            note.length = 0.25;
        else
            note.length = std::max(1.0 / 64.0, clipBeatAt(audio, timing, found.start + found.length) - beat);
        note.length = std::min(note.length, audio.length - note.start);
        if (!found.tuning.empty()) {
            std::vector<ControlPoint> points;
            for (const auto &[seconds, semitones] : found.tuning)
                points.push_back({std::max(0.0, clipBeatAt(audio, timing, found.start + seconds) - beat), semitones});
            note.setCurve(Expression::Tuning, std::move(points));
        }
        clip.notes.push_back(std::move(note));
    }
    std::stable_sort(clip.notes.begin(), clip.notes.end(), [](const Note &a, const Note &b) { return a.start < b.start; });
    return clip;
}

Clip chordClip(const Project &project, const Clip &audio, const std::vector<audio::TranscribedChord> &chords)
{
    Clip clip(audio.name + " " + conversionName(Conversion::Harmony), audio.start, audio.length);
    for (const audio::TranscribedChord &chord : chords) {
        const double from = std::max(0.0, clipBeatAt(audio, project.timing, chord.start) - audio.start);
        const double to = std::min(audio.length, clipBeatAt(audio, project.timing, chord.end) - audio.start);
        if (to - from < 1.0 / 64.0)
            continue;
        for (int key : chord.keys())
            clip.notes.push_back(Note{from, to - from, key, 0.7f});
    }
    return clip;
}

int addConvertedTrack(Project &project, int track, Clip clip, const audio::DeviceSetting &instrument)
{
    const Track *source = project.track(track);
    if (!source)
        return -1;
    Track converted;
    converted.kind = TrackKind::Instrument;
    converted.name = clip.name;
    converted.output = source->output;
    converted.instrument = instrument;
    converted.clips.push_back(std::move(clip));
    const auto at = static_cast<size_t>(track) + 1;
    project.tracks.insert(project.tracks.begin() + static_cast<std::ptrdiff_t>(at), std::move(converted));
    return static_cast<int>(at);
}

}
