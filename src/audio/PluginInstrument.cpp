#include "PluginInstrument.h"

#include <algorithm>

#include "PluginEffect.h"

namespace audio {

// PluginInstrument

PluginInstrument::PluginInstrument(plugin::PluginManager *manager, const DeviceSetting &setting)
    : InstrumentDevice(setting.name), id_(setting.id)
{
    if (manager)
        slot_ = std::make_unique<plugin::Slot>(*manager, setting.id, setting.name);
    applySetting(setting);
}

bool PluginInstrument::matches(const DeviceSetting &setting) const { return setting.kind == DeviceKind::Plugin && setting.id == id_; }

void PluginInstrument::applySetting(const DeviceSetting &setting)
{
    if (slot_)
        slot_->apply(setting.values, setting.state);
}

void PluginInstrument::update()
{
    if (slot_)
        slot_->update();
}

std::vector<DeviceParam> PluginInstrument::params() const { return slotParams(slot_.get()); }

std::string PluginInstrument::valueText(const DeviceParam &param, double value) const
{
    return slotValueText(slot_.get(), param, value);
}

uint32_t PluginInstrument::latency() const { return slot_ && slot_->live() ? slot_->latency() : 0; }

void PluginInstrument::prepare(double sampleRate, uint32_t maxFrames)
{
    for (auto &s : scratch_)
        s.assign(std::max<uint32_t>(maxFrames, plugin::kMaxFrames), 0.0f);
    if (slot_)
        slot_->setFormat(sampleRate);
}

bool PluginInstrument::push(plugin::EventKind kind, int key, float velocity, uint32_t frame)
{
    if (eventCount_ >= events_.size())
        return false; // more than a block can carry: the rest are dropped
    plugin::Event &e = events_[eventCount_++];
    e.kind = kind;
    e.frame = frame;
    e.id = static_cast<uint32_t>(std::clamp(key, 0, 127));
    e.channel = 0;
    e.value = velocity;
    return true;
}

void PluginInstrument::noteOnAt(int key, float velocity, uint32_t frame)
{
    const size_t k = static_cast<size_t>(std::clamp(key, 0, 127));
    if (push(plugin::EventKind::NoteOn, key, velocity, frame) && held_[k] < 255)
        ++held_[k];
}

void PluginInstrument::noteOffAt(int key, uint32_t frame)
{
    const size_t k = static_cast<size_t>(std::clamp(key, 0, 127));
    if (push(plugin::EventKind::NoteOff, key, 0.0f, frame) && held_[k] > 0)
        --held_[k];
}

void PluginInstrument::controlChangeAt(int controller, float value, uint32_t frame)
{
    if (eventCount_ >= events_.size() || controller < 0 || controller > kPitchBend)
        return;
    plugin::Event &e = events_[eventCount_++];
    e.kind = plugin::EventKind::Controller;
    e.frame = frame;
    e.id = static_cast<uint32_t>(controller);
    e.channel = 0;
    e.value = std::clamp(value, 0.0f, 1.0f);
}

void PluginInstrument::allNotesOff()
{
    // Each sounding key by name, then the plugin's own all notes off for anything else, all at
    // the time of the last event so far (events stay in order)
    const uint32_t frame = eventCount_ > 0 ? events_[eventCount_ - 1].frame : 0;
    for (size_t k = 0; k < held_.size(); ++k)
        for (; held_[k] > 0; --held_[k])
            push(plugin::EventKind::NoteOff, static_cast<int>(k), 0.0f, frame);
    push(plugin::EventKind::AllNotesOff, 0, 0.0f, frame);
}

void PluginInstrument::process(AudioBlock &block)
{
    const uint32_t count = eventCount_;
    eventCount_ = 0;
    if (!slot_ || !slot_->live() || block.channelCount == 0 || scratch_[0].empty()) {
        held_.fill(0); // loading, or none: silent (its notes are let go)
        return;
    }
    const uint32_t frames = std::min<uint32_t>(block.frames, static_cast<uint32_t>(scratch_[0].size()));
    float *out[2] = {scratch_[0].data(), scratch_[1].data()};
    plugin::ProcessBlock request;
    request.outputs = out;
    request.outputChannels = 2;
    request.transport = pluginTransport(&block.transport, block.sampleRate);
    // A block longer than a plugin takes at once goes in pieces, each with its own events
    uint32_t next = 0;
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = std::min(frames - done, plugin::kMaxFrames);
        uint32_t first = next;
        while (next < count && events_[next].frame < done + n)
            events_[next++].frame -= done;
        float *part[2] = {out[0] + done, out[1] + done};
        request.outputs = part;
        request.frames = n;
        request.events = next > first ? &events_[first] : nullptr;
        request.eventCount = next - first;
        slot_->process(request);
        done += n;
    }
    const uint32_t channels = std::min<uint32_t>(block.channelCount, 2);
    for (uint32_t c = 0; c < channels; ++c) {
        float *to = block.channels[c];
        const float *from = out[c];
        for (uint32_t i = 0; i < frames; ++i)
            to[i] += from[i];
    }
}

// SamplerDevice

SamplerDevice::SamplerDevice(const DeviceSetting &setting) : InstrumentDevice(setting.id), id_(setting.id), sampler_(setting.id)
{
    applySetting(setting);
}

DeviceSetting SamplerDevice::setting(const std::string &instrument)
{
    DeviceSetting s;
    s.kind = DeviceKind::Sampler;
    s.id = instrument;
    s.name = instrument;
    s.setValue(kLevel, kDefaultLevel);
    return s;
}

bool SamplerDevice::matches(const DeviceSetting &setting) const { return setting.kind == DeviceKind::Sampler && setting.id == id_; }

void SamplerDevice::applySetting(const DeviceSetting &setting)
{
    sampler_.gain.set(fromDecibels(static_cast<float>(setting.value(kLevel, kDefaultLevel))));
}

std::vector<DeviceParam> SamplerDevice::params() const
{
    DeviceParam level;
    level.id = kLevel;
    level.name = "Level";
    level.min = -40.0;
    level.max = 6.0;
    level.def = kDefaultLevel;
    level.unit = ParamUnit::Decibels;
    return {level};
}

}
