#include "Midi.h"

#include <algorithm>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace audio {

MidiMessage MidiMessage::noteOn(int key, int velocity)
{
    return {0x90, static_cast<uint8_t>(std::clamp(key, 0, 127)), static_cast<uint8_t>(std::clamp(velocity, 1, 127)), 0};
}

MidiMessage MidiMessage::noteOff(int key)
{
    return {0x80, static_cast<uint8_t>(std::clamp(key, 0, 127)), 0, 0};
}

// The driver's callback, on its own thread: channel messages go into the slot's queue
struct MidiCallback {
#ifdef _WIN32
    static void CALLBACK run(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR)
    {
        if (message == MIM_DATA)
            MidiHub::deviceMessage(*reinterpret_cast<MidiHub::Slot *>(instance), static_cast<uint32_t>(param1));
    }
#endif
};

void MidiHub::deviceMessage(Slot &slot, uint32_t message)
{
    MidiMessage m;
    m.status = static_cast<uint8_t>(message & 0xFF);
    m.data1 = static_cast<uint8_t>((message >> 8) & 0x7F);
    m.data2 = static_cast<uint8_t>((message >> 16) & 0x7F);
    m.source = slot.source;
    // Channel messages only; clock, active sensing and sysex aren't used
    if (m.status < 0x80 || m.status >= 0xF0)
        return;
    slot.queue.push(m); // a full queue drops it rather than block the driver
    slot.activity.fetch_add(1, std::memory_order_relaxed);
}

MidiHub::MidiHub() : AudioProcessor("MIDI")
{
    // Every slot exists from the start, so the audio thread never sees one being made
    for (auto &slot : slots_)
        slot = std::make_unique<Slot>();
    refresh();
}

MidiHub::~MidiHub()
{
    for (size_t i = 0; i < kMaxDevices; ++i)
        close(i);
}

bool MidiHub::refresh()
{
    std::vector<std::string> names;
#ifdef _WIN32
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count && names.size() < kMaxDevices; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            names.push_back("MIDI input " + std::to_string(i + 1));
            continue;
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nullptr, 0, nullptr, nullptr);
        std::string name(static_cast<size_t>(std::max(0, n - 1)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, name.data(), n, nullptr, nullptr);
        names.push_back(name);
    }
#endif
    if (names == names_)
        return false;
    // Indices may have moved: close everything; the caller reopens by name
    for (size_t i = 0; i < kMaxDevices; ++i)
        close(i);
    names_ = std::move(names);
    return true;
}

bool MidiHub::open(size_t index, std::string &error)
{
    if (index >= names_.size()) {
        error = "No such MIDI input";
        return false;
    }
    if (isOpen(index))
        return true;
    Slot &slot = *slots_[index];
    slot.name = names_[index];
    slot.source = static_cast<uint8_t>(index);
#ifdef _WIN32
    HMIDIIN handle = nullptr;
    MMRESULT result = midiInOpen(&handle, static_cast<UINT>(index), reinterpret_cast<DWORD_PTR>(&MidiCallback::run),
                                 reinterpret_cast<DWORD_PTR>(&slot), CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        error = "Couldn't open " + slot.name + (result == MMSYSERR_ALLOCATED ? " (another program is using it)" : "");
        return false;
    }
    midiInStart(handle);
    slot.handle = handle;
    return true;
#else
    error = "MIDI input isn't supported on this system yet";
    return false;
#endif
}

void MidiHub::close(size_t index)
{
    if (index >= kMaxDevices || !slots_[index] || !slots_[index]->handle)
        return;
#ifdef _WIN32
    auto handle = static_cast<HMIDIIN>(slots_[index]->handle);
    midiInStop(handle);
    midiInReset(handle);
    midiInClose(handle); // no callbacks after this returns
#endif
    slots_[index]->handle = nullptr;
}

bool MidiHub::isOpen(size_t index) const
{
    return index < kMaxDevices && slots_[index] && slots_[index]->handle;
}

uint64_t MidiHub::activity(size_t index) const
{
    return index < kMaxDevices && slots_[index] ? slots_[index]->activity.load(std::memory_order_relaxed) : 0;
}

std::string MidiHub::openNames() const
{
    std::string out;
    for (size_t i = 0; i < names_.size(); ++i) {
        if (isOpen(i))
            out += (out.empty() ? "" : ", ") + names_[i];
    }
    return out;
}

bool MidiHub::send(const MidiMessage &message)
{
    MidiMessage m = message;
    m.source = kVirtualSource;
    return virtual_.push(m);
}

bool MidiHub::takeStamped(StampedMidi &out)
{
    return stamped_.pop(out);
}

void MidiHub::process(AudioBlock &block)
{
    eventCount_ = 0;
    const TransportState &t = block.transport;
    auto take = [&](const MidiMessage &m) {
        if (eventCount_ < events_.size())
            events_[eventCount_++] = m;
        stamped_.push({m, t.position, t.playing, t.sampleRate, t.lap});
    };
    MidiMessage m;
    while (virtual_.pop(m))
        take(m);
    for (auto &slot : slots_) {
        if (!slot)
            continue;
        while (slot->queue.pop(m))
            take(m);
    }
}

}
