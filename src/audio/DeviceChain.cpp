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

void DeviceChain::apply(const std::vector<DeviceSetting> &midiEffects, const DeviceSetting &instrument,
                        const std::vector<DeviceSetting> &effects)
{
    const size_t n = std::min(effects.size(), kMaxEffects);
    const size_t m = std::min(midiEffects.size(), kMaxMidiEffects);
    const bool sameInstrument = instrument.empty() ? !instrument_ : instrument_ && instrument_->matches(instrument);
    bool same = sameInstrument && n == effects_.size() && m == midiEffects_.size();
    for (size_t i = 0; same && i < n; ++i)
        same = effects_[i]->matches(effects[i]);
    for (size_t i = 0; same && i < m; ++i)
        same = midiEffects_[i]->matches(midiEffects[i]);

    if (same) {
        if (instrument_)
            instrument_->applySetting(instrument);
        for (size_t i = 0; i < n; ++i)
            effects_[i]->applySetting(effects[i]);
        for (size_t i = 0; i < m; ++i)
            midiEffects_[i]->applySetting(midiEffects[i]);
        return;
    }

    const uint64_t stamp = blocks_.load(std::memory_order_acquire);
    std::vector<std::unique_ptr<Device>> gone;

    // The MIDI effects, kept by kind and id like the effects (so an arpeggio's held notes stay)
    {
        std::vector<std::unique_ptr<MidiEffect>> oldMidi = std::move(midiEffects_);
        midiEffects_.clear();
        for (size_t i = 0; i < m; ++i) {
            std::unique_ptr<MidiEffect> effect;
            for (auto &candidate : oldMidi) {
                if (candidate && candidate->matches(midiEffects[i])) {
                    effect = std::move(candidate);
                    break;
                }
            }
            if (!effect)
                effect = makeMidiEffect(midiEffects[i]);
            effect->applySetting(midiEffects[i]);
            midiEffects_.push_back(std::move(effect));
        }
        for (auto &effect : oldMidi)
            gone.push_back(std::move(effect));
    }

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

    // The curves and routes of devices that went go with them; those kept keep theirs
    for (const auto &effect : old) {
        curves_.erase(effect.get());
        routes_.erase(effect.get());
    }
    for (const auto &device : gone) {
        curves_.erase(device.get());
        routes_.erase(device.get());
    }
    publish(stamp);
    for (auto &effect : old)
        retire(std::move(effect), stamp);
    for (auto &device : gone)
        retire(std::move(device), stamp);
}

void DeviceChain::publish(uint64_t stamp)
{
    auto curvesOf = [this](const Device *device) -> std::shared_ptr<const AutomationCurves> {
        const auto found = curves_.find(device);
        return found != curves_.end() ? found->second : nullptr;
    };
    auto fresh = std::make_unique<Layout>();
    fresh->instrument = instrument_.get();
    for (size_t i = 0; i < midiEffects_.size(); ++i)
        fresh->midiEffects[i] = midiEffects_[i].get();
    fresh->midiCount = midiEffects_.size();
    fresh->instrumentCurves = curvesOf(instrument_.get());
    for (size_t i = 0; i < effects_.size(); ++i) {
        fresh->effects[i] = effects_[i].get();
        fresh->effectCurves[i] = curvesOf(effects_[i].get());
    }
    fresh->count = effects_.size();
    fresh->mixCurves = mixCurves_;
    auto routesOf = [this](const Device *device) -> std::shared_ptr<const ResolvedRoutes> {
        const auto found = routes_.find(device);
        return found != routes_.end() ? found->second : nullptr;
    };
    for (size_t i = 0; i < modulators_.size() && i < kMaxModulators; ++i)
        fresh->modulators[i] = modulators_[i].second.get();
    fresh->modulatorCount = std::min(modulators_.size(), kMaxModulators);
    fresh->instrumentRoutes = routesOf(instrument_.get());
    for (size_t i = 0; i < effects_.size(); ++i)
        fresh->effectRoutes[i] = routesOf(effects_[i].get());
    fresh->mixRoutes = mixRoutes_;
    published_.store(fresh.get(), std::memory_order_release);
    retired_.push_back({std::move(layout_), nullptr, stamp});
    layout_ = std::move(fresh);
}

void DeviceChain::setAutomation(const std::vector<std::pair<const Device *, AutomationCurves>> &devices, AutomationCurves mix)
{
    curves_.clear();
    for (const auto &[device, curves] : devices) {
        if (device && !curves.empty())
            curves_[device] = std::make_shared<const AutomationCurves>(curves);
    }
    mixCurves_ = mix.empty() ? nullptr : std::make_shared<const AutomationCurves>(std::move(mix));
    publish(blocks_.load(std::memory_order_acquire));
}

void DeviceChain::setModulation(const std::vector<Modulator> &modulators,
                                const std::vector<std::pair<const Device *, ResolvedRoutes>> &devices, ResolvedRoutes mix)
{
    const uint64_t stamp = blocks_.load(std::memory_order_acquire);
    // Each modulator keeps its state while one with its id and kind stays
    std::vector<std::pair<uint32_t, std::unique_ptr<ModulatorState>>> old = std::move(modulators_);
    modulators_.clear();
    for (const Modulator &m : modulators) {
        if (modulators_.size() >= kMaxModulators)
            break;
        ModulatorKind kind;
        if (!modulatorFromKey(m.setting.id, kind))
            continue;
        std::unique_ptr<ModulatorState> state;
        for (auto &candidate : old) {
            if (candidate.second && candidate.first == m.id && candidate.second->kind() == kind) {
                state = std::move(candidate.second);
                break;
            }
        }
        if (!state)
            state = std::make_unique<ModulatorState>(kind);
        state->apply(m.setting);
        modulators_.emplace_back(m.id, std::move(state));
    }
    // The routes, grouped by parameter so each is worked out once
    auto sorted = [](ResolvedRoutes routes) {
        std::stable_sort(routes.begin(), routes.end(), [](const ResolvedRoute &a, const ResolvedRoute &b) { return a.param < b.param; });
        return routes;
    };
    routes_.clear();
    for (const auto &[device, routes] : devices) {
        if (device && !routes.empty())
            routes_[device] = std::make_shared<const ResolvedRoutes>(sorted(routes));
    }
    mixRoutes_ = mix.empty() ? nullptr : std::make_shared<const ResolvedRoutes>(sorted(std::move(mix)));
    publish(stamp);
    for (auto &entry : old) {
        if (entry.second)
            retired_.push_back({nullptr, nullptr, stamp, std::move(entry.second)});
    }
}

float DeviceChain::modulatorValue(size_t slot) const
{
    return slot < modulators_.size() ? modulators_[slot].second->shown() : 0.0f;
}

void DeviceChain::evaluate(const Layout &layout, const TransportState &transport) const
{
    const uint64_t block = blocks_.load(std::memory_order_relaxed);
    if (evaluated_ == block || layout.modulatorCount == 0)
        return;
    evaluated_ = block;
    ModClock clock;
    clock.tempo = transport.tempo > 0.0 ? transport.tempo : 120.0;
    if (transport.playing) {
        clock.beat = transport.beatAt(0);
        clock.seconds = transport.sampleRate > 0.0 ? static_cast<double>(transport.position) / transport.sampleRate : 0.0;
    } else {
        clock.beat = freeBeat_;
        clock.seconds = freeSeconds_;
    }
    for (size_t i = 0; i < layout.modulatorCount; ++i) {
        modValues_[i] = layout.modulators[i]->value(clock);
        layout.modulators[i]->show(modValues_[i]);
    }
}

void DeviceChain::modulate(Device &device, const ResolvedRoutes &routes, const AutomationCurves *curves, double beat) const
{
    for (size_t i = 0; i < routes.size();) {
        size_t end = i + 1;
        while (end < routes.size() && routes[end].param == routes[i].param)
            ++end;
        // Its automation's value now, else the setting's
        double value = routes[i].base;
        if (curves) {
            for (const AutomationCurve &curve : *curves) {
                if (curve.param == routes[i].param) {
                    value = curve.valueAt(beat);
                    break;
                }
            }
        }
        device.automate(routes[i].param, modulatedValue(value, routes[i].curve, &routes[i], end - i, modValues_.data()));
        i = end;
    }
}

namespace {

// Sets a device's automated parameters for a block starting at `beat`
void automateDevice(Device &device, const AutomationCurves &curves, double beat)
{
    for (const AutomationCurve &curve : curves)
        device.automate(curve.param, curve.valueAt(beat));
}

}

void DeviceChain::automateInstrument(const TransportState &transport) const
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    if (layout && layout->instrument && layout->instrumentCurves)
        automateDevice(*layout->instrument, *layout->instrumentCurves, transport.beatAt(0));
    if (layout && layout->instrument && layout->instrumentRoutes) {
        evaluate(*layout, transport);
        modulate(*layout->instrument, *layout->instrumentRoutes, layout->instrumentCurves.get(), transport.beatAt(0));
    }
}

void DeviceChain::automateMix(const TransportState &transport, float &gain, float &pan) const
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    if (!layout)
        return;
    const double beat = transport.beatAt(0);
    if (layout->mixCurves) {
        for (const AutomationCurve &curve : *layout->mixCurves) {
            if (curve.param == kVolumeParam)
                gain = static_cast<float>(curve.valueAt(beat));
            else if (curve.param == kPanParam)
                pan = static_cast<float>(curve.valueAt(beat));
        }
    }
    // The modulators' share, from this block's values (worked out as the chain ran)
    if (layout->mixRoutes) {
        const ResolvedRoutes &routes = *layout->mixRoutes;
        for (size_t i = 0; i < routes.size();) {
            size_t end = i + 1;
            while (end < routes.size() && routes[end].param == routes[i].param)
                ++end;
            float &target = routes[i].param == kVolumeParam ? gain : pan;
            if (routes[i].param == kVolumeParam || routes[i].param == kPanParam)
                target = static_cast<float>(modulatedValue(target, routes[i].curve, &routes[i], end - i, modValues_.data()));
            i = end;
        }
    }
}

bool DeviceChain::automateMixParam(const TransportState &transport, uint32_t param, float &value) const
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    if (!layout || !layout->mixCurves)
        return false;
    for (const AutomationCurve &curve : *layout->mixCurves) {
        if (curve.param == param) {
            value = static_cast<float>(curve.valueAt(transport.beatAt(0)));
            return true;
        }
    }
    return false;
}

void DeviceChain::setKey(size_t effect, const float *left, const float *right)
{
    if (effect < kMaxEffects) {
        keyLeft_[effect] = left;
        keyRight_[effect] = right;
    }
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

size_t DeviceChain::liveMidiEffects(MidiEffect *const *&effects) const
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    if (!layout) {
        effects = nullptr;
        return 0;
    }
    effects = layout->midiEffects.data();
    return layout->midiCount;
}

void DeviceChain::process(float *left, float *right, uint32_t frames, const TransportState *transport)
{
    const Layout *layout = published_.load(std::memory_order_acquire);
    // The modulators: this block's values, and the level of what the track played for the follower
    if (layout && layout->modulatorCount > 0 && transport) {
        evaluate(*layout, *transport);
        float peak = 0.0f;
        for (uint32_t i = 0; i < frames; ++i)
            peak = std::max(peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
        const double seconds = transport->sampleRate > 0.0 ? frames / transport->sampleRate : 0.0;
        for (size_t m = 0; m < layout->modulatorCount; ++m)
            layout->modulators[m]->follow(peak, seconds);
        if (!transport->playing) {
            freeSeconds_ += seconds;
            freeBeat_ += seconds * (transport->tempo > 0.0 ? transport->tempo : 120.0) / 60.0;
        }
    }
    for (size_t i = 0; layout && i < layout->count; ++i) {
        if (transport && layout->effectCurves[i])
            automateDevice(*layout->effects[i], *layout->effectCurves[i], transport->beatAt(0));
        if (transport && layout->effectRoutes[i])
            modulate(*layout->effects[i], *layout->effectRoutes[i], layout->effectCurves[i].get(), transport->beatAt(0));
        layout->effects[i]->setKey(keyLeft_[i], keyRight_[i]);
        layout->effects[i]->run(left, right, frames, transport);
        layout->effects[i]->setKey(nullptr, nullptr);
    }
    keyLeft_.fill(nullptr);
    keyRight_.fill(nullptr);
    blocks_.fetch_add(1, std::memory_order_release);
}

// ChainInstrument

void ChainInstrument::automateBlock(const TransportState &transport)
{
    if (const DeviceChain *chain = chain_.load(std::memory_order_acquire))
        chain->automateInstrument(transport);
}

void ChainInstrument::beginBlock()
{
    // A replaced instrument (or MIDI effect) is kept until the chain has finished two more blocks, so
    // this block's pointers stay good to its end
    const DeviceChain *chain = chain_.load(std::memory_order_acquire);
    current_ = chain ? chain->liveInstrument() : nullptr;
    midiCount_ = chain ? chain->liveMidiEffects(midi_) : 0;
    in_.clear();
    if (current_)
        current_->beginBlock();
}

void ChainInstrument::queue(bool on, int key, float velocity, uint32_t frame)
{
    in_.push({frame, on, key, velocity});
}

void ChainInstrument::render(AudioBlock &block, uint32_t from, uint32_t to)
{
    if (to <= from || !current_)
        return;
    float *channels[2] = {block.channels[0] + from, block.channelCount > 1 ? block.channels[1] + from : nullptr};
    AudioBlock part{channels, std::min<uint32_t>(block.channelCount, 2), to - from, block.sampleRate, block.transport};
    current_->process(part);
}

void ChainInstrument::process(AudioBlock &block)
{
    const uint64_t counted = freeSamples_;
    freeSamples_ += block.frames;
    if (midiCount_ == 0) {
        if (current_)
            current_->process(block);
        return;
    }
    // The MIDI effects' clock: the song's while it plays, else samples counted at the tempo
    const TransportState &t = block.transport;
    MidiClock clock;
    clock.frames = block.frames;
    clock.sampleRate = block.sampleRate;
    if (t.playing) {
        clock.start = t.position;
        clock.beat0 = t.beatAt(0);
        clock.beatsPerFrame = block.frames > 0 ? (t.beatAt(block.frames) - clock.beat0) / block.frames : 0.0;
    } else {
        const double perFrame = (t.tempo > 0.0 ? t.tempo : 120.0) / 60.0 / block.sampleRate;
        clock.start = counted;
        clock.beat0 = static_cast<double>(counted) * perFrame;
        clock.beatsPerFrame = perFrame;
    }
    // Through each effect in turn
    in_.sort();
    const MidiEventList *from = &in_;
    MidiEventList *to = &a_;
    for (size_t i = 0; i < midiCount_; ++i) {
        to->clear();
        midi_[i]->run(*from, *to, clock);
        to->sort();
        from = to;
        to = to == &a_ ? &b_ : &a_;
    }
    const MidiEventList &out = *from;
    // To the instrument: every note with its frame, or the block cut at each one
    if (!current_)
        return;
    if (current_->timedEvents()) {
        for (size_t i = 0; i < out.count; ++i) {
            const MidiEvent &e = out.events[i];
            const uint32_t frame = std::min(e.frame, block.frames > 0 ? block.frames - 1 : 0);
            if (e.on)
                current_->noteOnAt(e.key, e.velocity, frame);
            else
                current_->noteOffAt(e.key, frame);
        }
        current_->process(block);
        return;
    }
    uint32_t cursor = 0;
    for (size_t i = 0; i < out.count; ++i) {
        const MidiEvent &e = out.events[i];
        const uint32_t frame = std::min(e.frame, block.frames);
        render(block, cursor, frame);
        cursor = std::max(cursor, frame);
        if (e.on)
            current_->noteOn(e.key, e.velocity);
        else
            current_->noteOff(e.key);
    }
    render(block, cursor, block.frames);
}

void ChainInstrument::noteOn(int key, float velocity)
{
    if (midiCount_ > 0)
        queue(true, key, velocity, 0);
    else if (current_)
        current_->noteOn(key, velocity);
}

void ChainInstrument::noteOff(int key)
{
    if (midiCount_ > 0)
        queue(false, key, 0.0f, 0);
    else if (current_)
        current_->noteOff(key);
}

void ChainInstrument::noteOnAt(int key, float velocity, uint32_t frame)
{
    if (midiCount_ > 0)
        queue(true, key, velocity, frame);
    else if (current_)
        current_->noteOnAt(key, velocity, frame);
}

void ChainInstrument::noteOffAt(int key, uint32_t frame)
{
    if (midiCount_ > 0)
        queue(false, key, 0.0f, frame);
    else if (current_)
        current_->noteOffAt(key, frame);
}

void ChainInstrument::allNotesOff()
{
    // The MIDI effects forget what they hold and what waits (an echo, an arpeggio's next step)
    for (size_t i = 0; i < midiCount_; ++i)
        midi_[i]->reset();
    in_.clear();
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

void ChainInstrument::noteExpressionAt(int key, int expression, float value, uint32_t frame)
{
    if (current_)
        current_->noteExpressionAt(key, expression, value, frame);
}

}
