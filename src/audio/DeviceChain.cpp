#include "DeviceChain.h"

#include <algorithm>

#include "PluginEffect.h"
#include "PluginInstrument.h"
#include "Synth.h"
#include "plugin/Slot.h"

namespace audio {

namespace {

// A plugin device's slot is waited for while the chain renders offline
void setSlotOffline(Device *device, bool offline)
{
    if (device && device->slot())
        device->slot()->setOffline(offline);
}

// An instrument that can't be made (an empty setting): silent
class SilentInstrument : public InstrumentDevice {
public:
    explicit SilentInstrument(const DeviceSetting &setting) : InstrumentDevice(setting.name), setting_(setting) {}
    DeviceKind kind() const override { return setting_.kind; }
    bool matches(const DeviceSetting &setting) const override { return setting.kind == setting_.kind && setting.id == setting_.id; }
    void applySetting(const DeviceSetting &) override {}
    std::vector<DeviceParam> params() const override { return {}; }
    void process(AudioBlock &) override {}
    void noteOn(int, float) override {}
    void noteOff(int) override {}
    void allNotesOff() override {}

private:
    DeviceSetting setting_;
};

}

std::unique_ptr<Effect> makeEffectDevice(const DeviceSetting &setting, plugin::PluginManager *plugins)
{
    if (setting.kind == DeviceKind::Plugin)
        return std::make_unique<PluginEffect>(plugins, setting);
    EffectType type = EffectType::Gain;
    effectFromKey(setting.id, type);
    std::unique_ptr<BuiltinEffect> effect = makeEffect(type);
    effect->applySetting(setting);
    return effect;
}

std::unique_ptr<InstrumentDevice> makeInstrumentDevice(const DeviceSetting &setting, plugin::PluginManager *plugins)
{
    switch (setting.kind) {
    case DeviceKind::Sampler: return std::make_unique<SamplerDevice>(setting);
    case DeviceKind::Plugin: return std::make_unique<PluginInstrument>(plugins, setting);
    case DeviceKind::Synth: return std::make_unique<SynthDevice>(setting);
    default: return std::make_unique<SilentInstrument>(setting);
    }
}

// DeviceChain

DeviceChain::DeviceChain() : layout_(std::make_unique<Layout>())
{
    published_.store(layout_.get(), std::memory_order_release);
}

DeviceChain::~DeviceChain() = default;

void DeviceChain::prepare(double sampleRate, uint32_t maxFrames)
{
    sampleRate_ = sampleRate;
    maxFrames_ = maxFrames;
    if (instrument_)
        instrument_->prepare(sampleRate, maxFrames);
    for (auto &effect : effects_)
        effect->prepare(sampleRate, maxFrames);
}

void DeviceChain::setOffline(bool offline)
{
    offline_ = offline;
    setSlotOffline(instrument_.get(), offline);
    for (auto &effect : effects_)
        setSlotOffline(effect.get(), offline);
}

Device *DeviceChain::device(int index)
{
    if (index == kInstrument)
        return instrument_.get();
    return index >= 0 ? effect(static_cast<size_t>(index)) : nullptr;
}

uint32_t DeviceChain::latency() const
{
    uint32_t total = instrument_ ? instrument_->latency() : 0;
    for (const auto &effect : effects_)
        total += effect->latency();
    return total;
}

void DeviceChain::retire(std::unique_ptr<Device> device, uint64_t stamp)
{
    if (device)
        retired_.push_back({nullptr, std::move(device), stamp});
}

void DeviceChain::apply(const DeviceSetting &instrument, const std::vector<DeviceSetting> &effects)
{
    const size_t n = std::min(effects.size(), kMaxEffects);
    const bool sameInstrument = instrument.empty() ? !instrument_ : instrument_ && instrument_->matches(instrument);
    bool same = sameInstrument && n == effects_.size();
    for (size_t i = 0; same && i < n; ++i)
        same = effects_[i]->matches(effects[i]);

    if (same) {
        if (instrument_)
            instrument_->applySetting(instrument);
        for (size_t i = 0; i < n; ++i)
            effects_[i]->applySetting(effects[i]);
        return;
    }

    const uint64_t stamp = blocks_.load(std::memory_order_acquire);
    std::vector<std::unique_ptr<Device>> gone;

    // The instrument: kept if it still matches, else made afresh
    if (!sameInstrument) {
        gone.push_back(std::move(instrument_));
        if (!instrument.empty()) {
            instrument_ = makeInstrumentDevice(instrument, plugins_);
            setSlotOffline(instrument_.get(), offline_);
            instrument_->prepare(sampleRate_, maxFrames_);
        }
    }
    if (instrument_)
        instrument_->applySetting(instrument);

    // A new order: keep each effect that still has a place (by kind and id), make the rest
    std::vector<std::unique_ptr<Effect>> old = std::move(effects_);
    effects_.clear();
    for (size_t i = 0; i < n; ++i) {
        std::unique_ptr<Effect> effect;
        for (auto &candidate : old) {
            if (candidate && candidate->matches(effects[i])) {
                effect = std::move(candidate);
                break;
            }
        }
        if (!effect) {
            effect = makeEffectDevice(effects[i], plugins_);
            setSlotOffline(effect.get(), offline_);
            effect->setBypass(effects[i].bypass);
            effect->prepare(sampleRate_, maxFrames_);
        }
        effects_.push_back(std::move(effect));
    }
    for (size_t i = 0; i < n; ++i)
        effects_[i]->applySetting(effects[i]);

    auto fresh = std::make_unique<Layout>();
    fresh->instrument = instrument_.get();
    for (size_t i = 0; i < n; ++i)
        fresh->effects[i] = effects_[i].get();
    fresh->count = n;
    published_.store(fresh.get(), std::memory_order_release);
    retired_.push_back({std::move(layout_), nullptr, stamp});
    layout_ = std::move(fresh);
    for (auto &effect : old)
        retire(std::move(effect), stamp);
    for (auto &device : gone)
        retire(std::move(device), stamp);
}

void DeviceChain::collect()
{
    if (instrument_) {
        instrument_->update();
        instrument_->collect();
    }
    for (auto &effect : effects_)
        effect->update();
    const uint64_t blocks = blocks_.load(std::memory_order_acquire);
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [&](const Retired &r) { return blocks >= r.stamp + 2; }),
                   retired_.end());
}

InstrumentDevice *DeviceChain::liveInstrument() const
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    return layout ? layout->instrument : nullptr;
}

void DeviceChain::process(float *left, float *right, uint32_t frames, const TransportState *transport)
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    for (size_t i = 0; layout && i < layout->count; ++i)
        layout->effects[i]->run(left, right, frames, transport);
    blocks_.fetch_add(1, std::memory_order_release);
}

// ChainInstrument

void ChainInstrument::beginBlock()
{
    // A replaced instrument is kept until the chain has finished two more blocks, so this block's
    // pointer stays good to its end
    const DeviceChain *chain = chain_.load(std::memory_order_acquire);
    current_ = chain ? chain->liveInstrument() : nullptr;
    if (current_)
        current_->beginBlock();
}

void ChainInstrument::process(AudioBlock &block)
{
    if (current_)
        current_->process(block);
}

void ChainInstrument::noteOn(int key, float velocity)
{
    if (current_)
        current_->noteOn(key, velocity);
}

void ChainInstrument::noteOff(int key)
{
    if (current_)
        current_->noteOff(key);
}

void ChainInstrument::noteOnAt(int key, float velocity, uint32_t frame)
{
    if (current_)
        current_->noteOnAt(key, velocity, frame);
}

void ChainInstrument::noteOffAt(int key, uint32_t frame)
{
    if (current_)
        current_->noteOffAt(key, frame);
}

void ChainInstrument::allNotesOff()
{
    if (current_)
        current_->allNotesOff();
}

void ChainInstrument::controlChange(int controller, float value)
{
    if (current_)
        current_->controlChange(controller, value);
}

void ChainInstrument::controlChangeAt(int controller, float value, uint32_t frame)
{
    if (current_)
        current_->controlChangeAt(controller, value, frame);
}

}
