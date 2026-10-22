#include "PluginEffect.h"

#include <algorithm>
#include <cmath>

namespace audio {

plugin::TransportInfo pluginTransport(const TransportState *transport, double sampleRate)
{
    plugin::TransportInfo info;
    if (!transport)
        return info;
    info.playing = transport->playing ? 1 : 0;
    info.tempo = transport->tempo;
    info.beats = transport->beatAt(0);
    if (const TempoMap *map = transport->map) {
        const Signature &signature = map->signatureAt(info.beats);
        info.beatsPerBar = static_cast<uint32_t>(signature.numerator);
        info.beatUnit = static_cast<uint32_t>(signature.denominator);
        info.bar = map->barAt(info.beats);
        info.barStart = map->barStart(info.bar);
        // BPM per beat, times beats per sample
        const double rate = transport->sampleRate > 0.0 ? transport->sampleRate : sampleRate;
        info.tempoPerSample = map->slopeAt(info.beats) * info.tempo / 60.0 / rate;
    } else {
        info.bar = static_cast<int32_t>(std::floor(info.beats / 4.0));
        info.barStart = info.bar * 4.0;
    }
    info.seconds = static_cast<double>(transport->position) / (transport->sampleRate > 0.0 ? transport->sampleRate : sampleRate);
    return info;
}

std::vector<DeviceParam> slotParams(const plugin::Slot *slot)
{
    std::vector<DeviceParam> out;
    if (!slot)
        return out;
    out.reserve(slot->params().size());
    for (const plugin::ParamInfo &info : slot->params())
        out.push_back(pluginParam(info));
    return out;
}

std::string slotValueText(plugin::Slot *slot, const DeviceParam &param, double value)
{
    if (!slot || param.unit == ParamUnit::Toggle)
        return formatParam(param, value);
    return slot->text(param.id, value);
}

PluginEffect::PluginEffect(plugin::PluginManager *manager, const DeviceSetting &setting) : id_(setting.id)
{
    if (manager)
        slot_ = std::make_unique<plugin::Slot>(*manager, setting.id, setting.name);
    applySetting(setting);
}

bool PluginEffect::matches(const DeviceSetting &setting) const { return setting.kind == DeviceKind::Plugin && setting.id == id_; }

void PluginEffect::applySetting(const DeviceSetting &setting)
{
    setBypass(setting.bypass);
    if (slot_)
        slot_->apply(setting.values, setting.state);
}

void PluginEffect::update()
{
    if (slot_)
        slot_->update();
}

std::vector<DeviceParam> PluginEffect::params() const { return slotParams(slot_.get()); }

std::string PluginEffect::valueText(const DeviceParam &param, double value) const
{
    return slotValueText(slot_.get(), param, value);
}

uint32_t PluginEffect::latency() const
{
    // A bypassed plugin isn't run, so it delays nothing
    return slot_ && !bypassed() && slot_->live() ? slot_->latency() : 0;
}

void PluginEffect::onPrepare(double sampleRate, uint32_t)
{
    if (slot_)
        slot_->setFormat(sampleRate);
}

void PluginEffect::automate(uint32_t paramId, double value)
{
    if (eventCount_ >= events_.size())
        return;
    plugin::Event &e = events_[eventCount_++];
    e.kind = plugin::EventKind::ParamValue;
    e.frame = 0;
    e.id = paramId;
    e.channel = 0;
    e.value = value;
}

void PluginEffect::process(float *left, float *right, uint32_t frames)
{
    const uint32_t count = eventCount_;
    eventCount_ = 0;
    if (!slot_ || !slot_->live())
        return; // loading, or not loadable: the sound passes through
    float *channels[2] = {left, right};
    plugin::ProcessBlock block;
    block.inputs = channels;
    block.inputChannels = 2;
    block.outputs = channels; // in place: the inputs are copied out before the outputs come back
    block.outputChannels = 2;
    block.transport = pluginTransport(transport_, sampleRate_);
    // Blocks are at most the AudioSystem's 4096 frames, the most a plugin takes at once
    for (uint32_t done = 0; done < frames;) {
        const uint32_t n = std::min(frames - done, plugin::kMaxFrames);
        float *part[2] = {left + done, right + done};
        block.inputs = part;
        block.outputs = part;
        block.frames = n;
        // The sidechain key (the chain hands a keyed effect its key for the block), the same piece of it
        const float *keyPart[2] = {keyLeft_ ? keyLeft_ + done : nullptr, keyRight_ ? keyRight_ + done : nullptr};
        block.keys = keyLeft_ ? keyPart : nullptr;
        block.keyChannels = keyLeft_ ? 2 : 0;
        // The automation's values go with the first piece
        block.events = done == 0 && count > 0 ? events_.data() : nullptr;
        block.eventCount = done == 0 ? count : 0;
        slot_->process(block);
        done += n;
    }
}

}
