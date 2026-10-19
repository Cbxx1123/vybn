#pragma once

#include <memory>

#include "Effects.h"
#include "plugin/Slot.h"

namespace audio {

// A CLAP plugin as an effect in a chain (DeviceKind::Plugin). The plugin runs in its own
// sandboxed host through a plugin::Slot, which loads it in the background: until it is ready the
// sound passes through untouched (as it does when the plugin can't be loaded at all, or without a
// PluginManager); once it has crashed or hung, or while it is late with a block, the effect is
// silent.
class PluginEffect : public Effect {
public:
    PluginEffect(plugin::PluginManager *manager, const DeviceSetting &setting);

    DeviceKind kind() const override { return DeviceKind::Plugin; }
    bool matches(const DeviceSetting &setting) const override;
    void applySetting(const DeviceSetting &setting) override;
    void update() override;
    std::vector<DeviceParam> params() const override;
    std::string valueText(const DeviceParam &param, double value) const override;
    plugin::Slot *slot() override { return slot_.get(); }
    uint32_t latency() const override;

protected:
    void onPrepare(double sampleRate, uint32_t maxFrames) override;
    void reset() override {}
    void process(float *left, float *right, uint32_t frames) override;

private:
    std::string id_;
    std::unique_ptr<plugin::Slot> slot_;
};

// The transport as a plugin is told it
plugin::TransportInfo pluginTransport(const TransportState *transport, double sampleRate);

// A plugin slot's parameters and value texts as a device's (shared by the plugin devices)
std::vector<DeviceParam> slotParams(const plugin::Slot *slot);
std::string slotValueText(plugin::Slot *slot, const DeviceParam &param, double value);

}
