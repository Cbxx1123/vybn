#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

#include "Instrument.h"
#include "Midi.h"
#include "env/SpscQueue.h"

namespace audio {

// A note on the timeline, in beats from its start
struct SequencedNote {
    double start = 0.0;
    double end = 0.0;
    int key = 60;
    float velocity = 0.8f;
};

// A controller's curve on the timeline: points in beats (sorted), values 0-1, joined by straight
// lines, or held until the next point when `stepped` (switches such as the sustain pedal). Before
// the first point it has the first point's value.
struct ControlCurve {
    struct Point {
        double beat = 0.0;
        float value = 0.0f;
    };
    int controller = 1; // MIDI controller 0-127, or Instrument::kPitchBend
    bool stepped = false;
    std::vector<Point> points;

    float valueAt(double beat) const;
};

// Everything a track plays, sorted by start
struct NoteSequence {
    std::vector<SequencedNote> notes;
    std::vector<ControlCurve> controls; // at most one per controller

    void sort();
};

// Plays a NoteSequence through an Instrument in time with the transport. Each block it finds
// the notes that start and end inside it and splits the block at their frames, so notes land on
// the exact sample. Stopping the transport or jumping it releases what is sounding. Notes that
// began before playback started aren't played from their middle. Controller curves are read at
// the start of each block and sent when their value changes (the sustain pedal holds the
// sequence's note offs until it lifts); stopping or jumping resets the bend and lifts the pedal.
//
// The sequence is swapped from the main thread (setSequence) through a queue, and the old one
// comes back for collect() to delete. audition/release play a key straight away, transport or
// not, for previewing notes from the editor. This is the source a TrackProcessor renders.
class NotePlayer : public AudioProcessor {
public:
    explicit NotePlayer(std::unique_ptr<Instrument> instrument);
    ~NotePlayer() override;

    Instrument &instrument() { return *instrument_; }

    // Main thread
    // Returns false, dropping it, when the audio thread hasn't taken the previous ones yet (no
    // device running): send it again later
    bool setSequence(std::unique_ptr<NoteSequence> sequence);
    void audition(int key, float velocity = 0.8f);
    void release(int key);
    // Live MIDI: the hub whose messages it plays while `listen` is set (set the hub before it
    // joins the graph; the hub must run before it in each block, and outlive it). Turning
    // `listen` off releases the keys it was holding. The sustain pedal (controller 64) holds
    // released keys until it lifts.
    void setMidiSource(const MidiHub *hub) { midi_ = hub; }
    std::atomic<bool> listen{false};
    // Frees retired sequences and whatever the instrument has finished with. Once a frame.
    void collect();

    void prepare(double sampleRate, uint32_t maxFrames) override;
    void process(AudioBlock &block) override;

private:
    struct Live {
        int key;
        float velocity; // 0 for a release
    };
    struct Sounding {
        int key;
        double end;
    };
    struct Event {
        uint32_t frame;
        bool on;
        int key;
        float velocity;
    };
    static constexpr size_t kMaxSounding = 128;
    static constexpr size_t kMaxEvents = 256;

    void allOff();
    void playControls(double beat);
    void resetControls();
    void render(AudioBlock &block, uint32_t from, uint32_t to);

    std::unique_ptr<Instrument> instrument_;
    env::SpscQueue<NoteSequence *, 16> incoming_;
    env::SpscQueue<NoteSequence *, 16> retired_;
    env::SpscQueue<Live, 64> live_;

    // Audio thread
    NoteSequence *current_ = nullptr;
    std::array<Sounding, kMaxSounding> sounding_{};
    size_t soundingCount_ = 0;
    std::array<Event, kMaxEvents> events_{};
    bool wasPlaying_ = false;
    const MidiHub *midi_ = nullptr;
    bool wasListening_ = false;
    bool sustain_ = false;
    std::bitset<128> held_;      // keys down on the keyboard
    std::bitset<128> sustained_; // released while the pedal was down
    void playMidi(const MidiMessage &m);
    void releaseMidi();
    uint64_t expected_ = 0; // where the transport should be if it didn't jump
    std::array<float, Instrument::kControllers> sent_{}; // the last value of each, -1 for none
    bool pedal_ = false;           // the sequence's sustain pedal
    std::bitset<128> pedalled_;    // the sequence's keys held by it
};

}
