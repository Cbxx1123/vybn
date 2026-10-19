#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "AudioProcessor.h"
#include "env/SpscQueue.h"

namespace audio {

// A MIDI channel message: status byte (type and channel) and two data bytes
struct MidiMessage {
    uint8_t status = 0;
    uint8_t data1 = 0; // the key, or the controller
    uint8_t data2 = 0; // the velocity, or the value
    uint8_t source = 0; // which device it came from (kVirtualSource for MidiHub::send)

    uint8_t type() const { return status & 0xF0; }
    bool isNoteOn() const { return type() == 0x90 && data2 > 0; }
    bool isNoteOff() const { return type() == 0x80 || (type() == 0x90 && data2 == 0); }
    bool isController(uint8_t number) const { return type() == 0xB0 && data1 == number; }
    static MidiMessage noteOn(int key, int velocity);
    static MidiMessage noteOff(int key);
};

// A message as the audio thread saw it: where the transport was when it arrived
struct StampedMidi {
    MidiMessage message;
    uint64_t position = 0; // samples on the timeline
    bool playing = false;
    double sampleRate = 48000.0;
    double tempo = 120.0;
};

// MIDI input: keyboards and controllers plugged in (Windows' MIDI API; elsewhere none are
// listed), plus messages the app sends itself. Add it to the AudioSystem before any track, so it
// runs first in every block: it collects what arrived since the last block, for NotePlayers that
// listen (events()) to play, and stamps each message with the transport's position for recording
// (takeStamped, on the main thread).
//
// Threads: the device driver's thread pushes into one lock-free queue per device; the audio
// thread drains them; the main thread opens and closes devices and reads the stamped messages.
// Messages land at the start of the block they arrive in, so timing is within a buffer.
class MidiHub : public AudioProcessor {
public:
    static constexpr size_t kMaxDevices = 8;
    static constexpr uint8_t kVirtualSource = 255;

    MidiHub();
    ~MidiHub() override;

    // Main thread: the devices plugged in now. refresh returns whether the list changed.
    const std::vector<std::string> &devices() const { return names_; }
    bool refresh();
    bool open(size_t index, std::string &error);
    void close(size_t index);
    bool isOpen(size_t index) const;
    // Messages a device has sent since it opened, so a light can flicker
    uint64_t activity(size_t index) const;
    // The open devices' names, joined, for showing which input is in use
    std::string openNames() const;

    // Main thread: a message from the app itself (an on-screen keyboard, a test)
    bool send(const MidiMessage &message);
    // Main thread: the next message the audio thread has stamped, for recording
    bool takeStamped(StampedMidi &out);

    // Audio thread, after this block's process (NotePlayers run after the hub)
    const MidiMessage *events() const { return events_.data(); }
    size_t eventCount() const { return eventCount_; }

    void process(AudioBlock &block) override;

private:
    struct Slot {
        void *handle = nullptr; // HMIDIIN
        std::string name;
        env::SpscQueue<MidiMessage, 1024> queue; // driver thread -> audio thread
        std::atomic<uint64_t> activity{0};
        uint8_t source = 0;
    };
    static void deviceMessage(Slot &slot, uint32_t message);
    friend struct MidiCallback;

    std::vector<std::string> names_;
    std::array<std::unique_ptr<Slot>, kMaxDevices> slots_; // by device index; all made up front
    env::SpscQueue<MidiMessage, 1024> virtual_;            // main -> audio
    env::SpscQueue<StampedMidi, 4096> stamped_;             // audio -> main

    std::array<MidiMessage, 1024> events_{};
    size_t eventCount_ = 0;
};

}
