#include "PluginEffect.h"

#include <algorithm>

namespace audio {

plugin::TransportInfo pluginTransport(const TransportState *transport, double sampleRate)
{
    plugin::TransportInfo info;
    if (!transport)
        return info;
    info.playing = transport->playing ? 1 : 0;
    info.tempo = transport->tempo;
    info.beats = transport->beatAt(0);
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

void PluginEffect::process(float *left, float *right, uint32_t frames)
{
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
        slot_->process(block);
        done += n;
    }
}

}
