#pragma once

#include <string>

#include "AudioProcessor.h"
#include "Device.h"

namespace audio {

// A sound engine played by notes: the interface every instrument engine implements (the
// Sampler today; synths later). It renders like any processor, adding into the block. Notes
// arrive between process calls: a NotePlayer splits each block at its notes' frames, so an
// instrument only has to start and stop voices when told and render what is sounding.
//
// Threads: beginBlock, noteOn, noteOff and allNotesOff are called on the audio thread, between process
// calls, so like process they must not lock, allocate or do I/O. Keys are MIDI note numbers
// (60 is middle C, C4); velocity is 0-1.
class Instrument : public AudioProcessor {
public:
    using AudioProcessor::AudioProcessor;

    virtual void noteOn(int key, float velocity) = 0;
    virtual void noteOff(int key) = 0;
    // Releases every sounding note (the transport stopped or jumped)
    virtual void allNotesOff() = 0;
    // Audio thread, at the start of each block before beginBlock: an instrument whose parameters
    // follow automation sets them for the block here, from the transport's position
    virtual void automateBlock(const TransportState & /*transport*/) {}
    // Audio thread, at the start of each block before any notes: pick up changes from the main
    // thread here (a new sample set, say)
    virtual void beginBlock() {}
    // Main thread, once a frame: frees what the audio thread has finished with
    virtual void collect() {}

    // Instruments that take each note with its frame in the block and render the whole block in
    // one go say so here (plugins, which would otherwise pay a round trip to their host for every
    // note); the NotePlayer then hands them the block's notes and processes it once instead of
    // splitting it at each note. Asked every block, so it may change.
    virtual bool timedEvents() const { return false; }
    virtual void noteOnAt(int key, float velocity, uint32_t /*frame*/) { noteOn(key, velocity); }
    virtual void noteOffAt(int key, uint32_t /*frame*/) { noteOff(key); }

    // A MIDI controller (0-127) or the pitch bend (kPitchBend), as 0-1 (the bend's middle is
    // 0.5), on the audio thread like notes. Instruments ignore those they don't use; the
    // NotePlayer handles the sustain pedal (64) itself, holding note offs.
    static constexpr int kPitchBend = 128;
    static constexpr int kControllers = 129;
    virtual void controlChange(int /*controller*/, float /*value*/) {}
    virtual void controlChangeAt(int controller, float value, uint32_t /*frame*/) { controlChange(controller, value); }

    // A sounding note's expression, on the audio thread like notes, numbered as daw::Expression
    // (kTuning: its tuning, in semitones; every expression rests at 0). Plugins take it against
    // the note's key; the other instruments ignore it.
    static constexpr int kTuning = 0;
    static constexpr int kExpressions = 1;
    virtual void noteExpressionAt(int /*key*/, int /*expression*/, float /*value*/, uint32_t /*frame*/) {}
};

// An instrument in a track's chain (a library instrument on the Sampler, or a plugin): an
// Instrument that is also a Device, so it is chosen, described and edited like every other device
class InstrumentDevice : public Instrument, public Device {
public:
    using Instrument::Instrument;
};

}
