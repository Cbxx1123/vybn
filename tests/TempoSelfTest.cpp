#include "TempoSelfTest.h"
#include "Harness.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "audio/TempoMap.h"
#include "daw/io/MidiExport.h"
#include "daw/io/ProjectFile.h"
#include "daw/io/Toml.h"
#include "daw/model/ExportModel.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"

namespace tests {

namespace {

bool near(double a, double b, double tolerance = 1e-9)
{
    return std::fabs(a - b) <= tolerance;
}

// The seconds to `beat` by adding up 60/bpm in small steps, to check the closed form against
double integrate(const audio::TempoMap &map, double beat)
{
    const int steps = 200000;
    const double h = beat / steps;
    double seconds = 0.0;
    for (int i = 0; i < steps; ++i)
        seconds += h * 60.0 / map.bpmAt((i + 0.5) * h);
    return seconds;
}

}

int runTempoChecks()
{
    Checker c;

    // One tempo and one signature give the old formulas' numbers
    {
        const audio::TempoMap map(96.0, 3, 4);
        const double rate = 48000.0;
        bool same = map.constant();
        for (double beat : {0.0, 0.25, 1.0 / 3.0, 7.5, 1000.125}) {
            same = same && near(map.samplesAt(beat, rate), beat * (rate * 60.0 / 96.0), 1e-6) &&
                   near(map.beatAtSample(beat * rate * 60.0 / 96.0, rate), beat, 1e-9) && map.bpmAt(beat) == 96.0;
            const long whole = static_cast<long>(std::floor(beat));
            const audio::BarPosition p = map.position(beat);
            same = same && p.bar == whole / 3 && p.unit == whole % 3 && near(p.fraction, beat - whole);
        }
        c.check(same, "Tempo map: a constant map matches the old formulas");
    }

    // Held tempos: 120 for 4 beats (2 s), then 60 (1 s a beat)
    audio::TempoMap steps;
    steps.setPoints({{0.0, 120.0, false}, {4.0, 60.0, false}});
    c.check(near(steps.secondsAt(4.0), 2.0) && near(steps.secondsAt(6.0), 4.0) && near(steps.beatAt(3.0), 5.0) &&
                near(steps.beatAt(1.0), 2.0) && steps.bpmAt(3.99) == 120.0 && steps.bpmAt(4.0) == 60.0,
            "Tempo map: held tempos convert both ways", std::to_string(steps.secondsAt(6.0)) + " s at beat 6");

    // A ramp from 60 to 180 over 8 beats, then held
    audio::TempoMap ramp;
    ramp.setPoints({{0.0, 60.0, true}, {8.0, 180.0, false}});
    const double rampEnd = ramp.secondsAt(8.0);
    c.check(near(rampEnd, integrate(ramp, 8.0), 1e-6) && near(ramp.secondsAt(5.0), integrate(ramp, 5.0), 1e-6),
            "Tempo map: a ramp's time matches adding it up", std::to_string(rampEnd) + " s over the ramp");
    bool roundTrip = true;
    for (double beat = 0.0; beat < 12.0; beat += 0.37)
        roundTrip = roundTrip && near(ramp.beatAt(ramp.secondsAt(beat)), beat, 1e-9);
    c.check(roundTrip && near(ramp.bpmAt(4.0), 120.0) && near(ramp.slopeAt(4.0), 15.0) && ramp.slopeAt(9.0) == 0.0 &&
                near(ramp.secondsAt(10.0), rampEnd + 2.0 * 60.0 / 180.0),
            "Tempo map: beats survive a round trip through a ramp");
    // A falling ramp
    audio::TempoMap falling;
    falling.setPoints({{0.0, 200.0, true}, {6.0, 50.0, false}});
    c.check(near(falling.secondsAt(6.0), integrate(falling, 6.0), 1e-6) && near(falling.beatAt(falling.secondsAt(5.5)), 5.5, 1e-9),
            "Tempo map: a falling ramp");
    // Before the start the first tempo holds
    c.check(near(ramp.secondsAt(-2.0), -2.0) && near(ramp.beatAt(-1.0), -1.0), "Tempo map: before beat 0 the first tempo holds");

    // Signatures: 4/4 for 2 bars, 7/8 for 3 bars, then 6/8
    audio::TempoMap metre;
    metre.setSignatures({{0, 4, 4}, {2, 7, 8}, {5, 6, 8}});
    c.check(near(metre.barStart(2), 8.0) && near(metre.barStart(3), 11.5) && near(metre.barStart(5), 18.5) &&
                near(metre.barStart(6), 21.5) && metre.barLength(3) == 3.5 && metre.barLength(7) == 3.0,
            "Tempo map: bars in mixed signatures", "bar 6 at beat " + std::to_string(metre.barStart(6)));
    bool barsBack = true;
    for (int bar = -2; bar < 12; ++bar)
        barsBack = barsBack && metre.barAt(metre.barStart(bar)) == bar && metre.barAt(metre.barStart(bar + 1) - 1e-6) == bar;
    c.check(barsBack, "Tempo map: each bar line is in the bar it starts");
    const audio::BarPosition p = metre.position(11.5 + 2.25); // bar 3 (7/8), 4.5 eighths in
    c.check(p.bar == 3 && p.unit == 4 && near(p.fraction, 0.5) && metre.signatureAt(13.0).numerator == 7,
            "Tempo map: a position in eighths", std::to_string(p.bar) + "." + std::to_string(p.unit));
    c.check(near(metre.nearestBar(10.0), 11.5) && near(metre.nearestBar(9.0), 8.0), "Tempo map: the nearest bar line");
    int counted = 0;
    double lastStart = -1.0;
    metre.forEachBar(9.0, 19.0, [&](int, double start, const audio::Signature &) {
        ++counted;
        lastStart = start;
    });
    c.check(counted == 4 && near(lastStart, 18.5), "Tempo map: the bars over a range", std::to_string(counted) + " bars");

    // Tidying
    audio::TempoMap tidy;
    tidy.setPoints({{8.0, 1000.0, true}, {4.0, 90.0, false}, {4.0, 100.0, false}, {-1.0, 5.0, false}});
    tidy.setSignatures({{3, 0, 7}, {3, 5, 4}, {1, 99, 16}});
    c.check(tidy.points().size() == 3 && tidy.points()[0].beat == 0.0 && tidy.points()[0].bpm == audio::TempoMap::kMinBpm &&
                tidy.points()[1].bpm == 90.0 && tidy.points()[2].bpm == audio::TempoMap::kMaxBpm &&
                tidy.signatures().size() == 3 && tidy.signatures()[0].bar == 0 && tidy.signatures()[0].numerator == 16 &&
                tidy.signatures()[2].numerator == 1 && tidy.signatures()[2].denominator == 8,
            "Tempo map: what it's given is clamped, sorted and started at 0");
    c.check(tidy != audio::TempoMap() && audio::TempoMap(120.0) == audio::TempoMap(), "Tempo map: maps compare by content");

    // The project file: a constant map is written as before (tempo and beats_per_bar alone); a map
    // with changes, ramps and an opening 6/8 comes back exactly
    {
        daw::Project plain;
        const std::string plainText = daw::writeProject(plain);
        c.check(plainText.find("tempo = " + daw::toml::number(120.0) + "\n") != std::string::npos &&
                    plainText.find("beats_per_bar = 4\n") != std::string::npos && plainText.find("tempo_changes") == std::string::npos &&
                    plainText.find("signature_changes") == std::string::npos && plainText.find("beat_unit") == std::string::npos,
                "Tempo map: a constant map is saved with the old keys only");
        daw::Project p;
        p.timing.setPoints({{0.0, 96.0, true}, {16.0, 140.0, false}, {1.0 / 3.0 + 20.0, 72.5, true}, {40.0, 80.0, false}});
        p.timing.setSignatures({{0, 6, 8}, {4, 7, 8}, {9, 4, 4}});
        const std::string text = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool ok = daw::readProject(text, back, error);
        c.check(ok && back.timing == p.timing && text.find("tempo = " + daw::toml::number(96.0) + "\n") != std::string::npos &&
                    text.find("beat_unit = 8\n") != std::string::npos,
                "Tempo map: changes, ramps and signatures survive the file", error);
        // Written by hand (after the file's header and format): entries at the start are ignored,
        // values clamped
        daw::Project odd;
        const std::string oddText = text.substr(0, text.find("name = ")) +
                                    "tempo = 1000\nbeats_per_bar = 40\nbeat_unit = 5\n"
                                    "tempo_changes = [ { beat = 0, bpm = 60 }, { beat = 8, bpm = 5 } ]\n"
                                    "signature_changes = [ { bar = 2, numerator = 3, denominator = 32 } ]\n";
        const bool oddOk = daw::readProject(oddText, odd, error);
        c.check(oddOk && odd.timing.tempo() == audio::TempoMap::kMaxBpm && odd.timing.points().size() == 2 &&
                    odd.timing.points()[1].bpm == audio::TempoMap::kMinBpm && odd.timing.firstSignature().numerator == 16 &&
                    odd.timing.firstSignature().denominator == 4 && odd.timing.signatures()[1].denominator == 16,
                "Tempo map: a file's odd values are tidied", error);
    }

    // History: what an edit of the map is called, and that it counts as a change
    {
        daw::Project a;
        daw::Project b = a;
        b.timing.setPoints({{0.0, 120.0, false}, {8.0, 90.0, false}});
        daw::Project s = a;
        s.timing.setSignatures({{0, 4, 4}, {2, 3, 4}});
        c.check(!daw::sameContent(a, b) && daw::describeChange(a, b) == "Change tempo" && !daw::sameContent(a, s) &&
                    daw::describeChange(a, s) == "Change time signature",
                "Tempo map: an edit is an undo step, named for what changed", daw::describeChange(a, s));
    }

    // An audio clip across a tempo change: 4 beats at 120 (2 s) then 60. Splitting at beat 6 starts
    // the right part 3 s into the sound; its length in beats follows the map.
    {
        daw::Project p;
        p.timing.setPoints({{0.0, 120.0, false}, {4.0, 60.0, false}});
        daw::Track audioTrack;
        audioTrack.kind = daw::TrackKind::Audio;
        daw::Clip clip("Take", 2.0, p.beatsFor(2.0, 4.0));
        clip.audioFile = "take.wav";
        audioTrack.clips.push_back(clip);
        p.tracks.push_back(audioTrack);
        p.select(0, 0);
        const bool lengthRight = near(clip.length, 5.0); // 1 s to beat 4, then 3 beats in 3 s
        const bool split = p.splitClip(6.0);
        c.check(lengthRight && split && near(p.tracks[0].clips[1].offset, 3.0), "Tempo map: splitting an audio clip across a tempo change",
                "offset " + std::to_string(p.tracks[0].clips[1].offset));
    }

    // Bars as the export ranges take them: bars 3 to 5 of 4/4 then 3/4 from bar 2
    {
        daw::Project p;
        p.timing.setSignatures({{0, 4, 4}, {2, 3, 4}});
        daw::ExportTarget target;
        target.range = daw::ExportRangeKind::Bars;
        target.fromBar = 3;
        target.toBar = 5;
        double s = -1.0, e = -1.0;
        const bool ranged = daw::exportTargetRange(p, target, s, e);
        c.check(ranged && s == 8.0 && e == 14.0, "Tempo map: an export's bars follow the signatures",
                std::to_string(s) + " to " + std::to_string(e));
    }

    // MIDI export: the conductor has a tempo and a signature at the start, and one for each change
    {
        daw::Project p;
        p.timing.setPoints({{0.0, 100.0, false}, {8.0, 150.0, false}});
        p.timing.setSignatures({{0, 4, 4}, {3, 7, 8}});
        daw::Track inst;
        inst.kind = daw::TrackKind::Instrument;
        inst.clips.push_back({"Riff", 0.0, 16.0, {{0.0, 1.0, 60, 0.8f}}});
        p.tracks.push_back(inst);
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path path = fs::temp_directory_path(ec) / "vybn-tempo-selftest.mid";
        std::string error;
        const bool written = daw::writeMidiFile(p, {}, path.u8string(), error);
        std::ifstream in(path, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        fs::remove(path, ec);
        auto count = [&](const std::string &pattern) {
            size_t n = 0;
            for (size_t at = bytes.find(pattern); at != std::string::npos; at = bytes.find(pattern, at + 1))
                ++n;
            return n;
        };
        const std::string sevenEight("\xFF\x58\x04\x07\x03\x18\x08", 7);
        const std::string fourFour("\xFF\x58\x04\x04\x02\x18\x08", 7);
        c.check(written && count(std::string("\xFF\x51\x03", 3)) == 2 && count(sevenEight) == 1 && count(fourFour) == 1,
                "Tempo map: MIDI export writes each tempo and signature change", error);
    }

    return c.failures;
}

}
