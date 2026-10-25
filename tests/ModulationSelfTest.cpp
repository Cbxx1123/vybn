#include "ModulationSelfTest.h"
#include "Harness.h"

#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "audio/DeviceChain.h"
#include "audio/Effects.h"
#include "audio/Modulation.h"
#include "daw/io/ProjectFile.h"
#include "daw/model/AutomationLanes.h"
#include "daw/model/Freeze.h"
#include "daw/model/History.h"
#include "daw/model/Project.h"

namespace tests {

namespace {

bool near(double a, double b, double tolerance = 1e-4)
{
    return std::fabs(a - b) <= tolerance;
}

std::string text(double v)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4g", v);
    return buffer;
}

// A modulator's state with some of its parameters set
std::unique_ptr<audio::ModulatorState> stateOf(audio::ModulatorKind kind, std::vector<std::pair<uint32_t, double>> values)
{
    audio::DeviceSetting s = audio::modulatorSetting(kind);
    for (const auto &[id, value] : values)
        s.setValue(id, value);
    auto state = std::make_unique<audio::ModulatorState>(kind);
    state->apply(s);
    return state;
}

float at(const std::unique_ptr<audio::ModulatorState> &state, double beat, double seconds = 0.0)
{
    audio::ModClock clock;
    clock.beat = beat;
    clock.seconds = seconds;
    return state->value(clock);
}

}

int runModulationAudioChecks()
{
    Checker c;
    using K = audio::ModulatorKind;

    // The LFO, synced to quarter notes: each shape at known beats, and unipolar; free at its rate in Hz
    {
        const auto sine = stateOf(K::Lfo, {});
        const auto triangle = stateOf(K::Lfo, {{0, 1}});
        const auto square = stateOf(K::Lfo, {{0, 4}});
        const auto saw = stateOf(K::Lfo, {{0, 2}});
        const auto unipolar = stateOf(K::Lfo, {{5, 1}});
        const auto free = stateOf(K::Lfo, {{2, 0}, {1, 2.0}});
        const auto shifted = stateOf(K::Lfo, {{4, 90}});
        const bool ok = near(at(sine, 0.25), 1.0) && near(at(sine, 0.75), -1.0) && near(at(triangle, 0.25), 1.0) &&
                        near(at(triangle, 0.5), 0.0) && near(at(square, 0.25), 1.0) && near(at(square, 0.75), -1.0) &&
                        near(at(saw, 0.5), 0.0) && near(at(unipolar, 0.0), 0.5) && near(at(free, 0.0, 0.125), 1.0) &&
                        near(at(shifted, 0.0), 1.0);
        c.check(ok, "Modulation: the LFO's shapes, polarity, phase, sync and free rate");
        const auto random = stateOf(K::Lfo, {{0, 5}});
        c.check(at(random, 0.1) == at(random, 0.9) && at(random, 1.1) != at(random, 0.1) && at(random, 3.5) == at(random, 3.5),
                "Modulation: sample and hold holds a draw a cycle, the same every time");
    }

    // Steps: four steps a beat each, gliding over the last half of each when asked
    {
        const auto steps = stateOf(K::Steps, {{0, 4}, {1, 4}});
        const auto glide = stateOf(K::Steps, {{0, 4}, {1, 4}, {2, 50}});
        const bool ok = near(at(steps, 0.1), 1.0) && near(at(steps, 1.5), 0.25) && near(at(steps, 2.0), 0.6) &&
                        near(at(steps, 4.2), 1.0) && near(at(glide, 0.25), 1.0) && near(at(glide, 0.75), 0.625);
        c.check(ok, "Modulation: Steps' values in turn, and the glide between them", text(at(glide, 0.75)));
    }

    // The follower: up at its attack, down at its release
    {
        auto follower = stateOf(K::Follower, {{0, 10}, {1, 100}});
        for (int i = 0; i < 50; ++i)
            follower->follow(1.0f, 0.001);
        const float up = at(follower, 0.0);
        for (int i = 0; i < 100; ++i)
            follower->follow(0.0f, 0.001);
        const float down = at(follower, 0.0);
        c.check(up > 0.99f && near(down, up * std::exp(-1.0f), 0.01), "Modulation: the follower rises at its attack, falls at its release",
                text(up) + " then " + text(down));
    }

    // In a chain: a gain effect's level moved by a square LFO, half its travel each way, and clamped
    {
        audio::DeviceChain chain;
        chain.prepare(48000.0, 256);
        chain.apply({audio::effectSetting(audio::EffectType::Gain)});
        audio::Modulator lfo{1, audio::modulatorSetting(K::Lfo)};
        lfo.setting.setValue(0, 4); // square
        const audio::DeviceParam param = audio::effectParam(audio::effectInfo(audio::EffectType::Gain).params[0], 0);
        audio::ResolvedRoute route;
        route.slot = 0;
        route.param = 0;
        route.curve = daw::automationCurve(param);
        route.base = -12.0;
        route.depth = 0.25f;
        chain.setModulation({lfo}, {{chain.effect(0), {route}}}, {});
        auto *gain = static_cast<audio::BuiltinEffect *>(chain.effect(0));
        std::vector<float> left(256, 0.5f), right(256, 0.5f);
        audio::TransportState t;
        t.playing = true;
        t.sampleRate = 48000.0;
        t.tempo = 120.0;
        auto levelAt = [&](uint64_t sample) {
            t.position = sample;
            chain.process(left.data(), right.data(), 256, &t);
            return gain->param(0);
        };
        const float high = levelAt(1000);  // beat 0.04: the square's top
        const float low = levelAt(13000);  // beat 0.54: its bottom
        const double basePosition = route.curve.position(-12.0);
        const bool moved = near(high, route.curve.value(basePosition + 0.25), 1e-3) && near(low, route.curve.value(basePosition - 0.25), 1e-3);
        route.depth = -1.0f;
        chain.setModulation({lfo}, {{chain.effect(0), {route}}}, {});
        const float clamped = levelAt(1000);
        c.check(moved && near(clamped, -60.0f, 1e-3) && chain.modulatorValue(0) == 1.0f,
                "Modulation: a routed LFO moves an effect's level each block, clamped at its ends",
                text(high) + " / " + text(low) + " / " + text(clamped));

        // Under automation: the lane's value is where the modulation starts from
        audio::AutomationCurve lane;
        lane.param = 0;
        lane.curve = route.curve;
        lane.points = {{0.0, static_cast<float>(route.curve.position(0.0))}};
        chain.setAutomation({{chain.effect(0), {lane}}}, {});
        route.depth = 0.25f;
        chain.setModulation({lfo}, {{chain.effect(0), {route}}}, {});
        const float automated = levelAt(1000);
        c.check(near(automated, route.curve.value(route.curve.position(0.0) + 0.25), 1e-3),
                "Modulation: under automation, the modulation moves the automated value", text(automated));
    }

    // The track's volume moved by a macro at full, pulled down by its route
    {
        audio::DeviceChain chain;
        chain.prepare(48000.0, 256);
        audio::Modulator macro{3, audio::modulatorSetting(K::Macro)};
        macro.setting.setValue(0, 100);
        audio::ResolvedRoute route;
        route.slot = 0;
        route.param = audio::kVolumeParam;
        route.curve = daw::automationCurve(audio::kVolumeParam);
        route.depth = -0.5f;
        chain.setModulation({macro}, {}, {route});
        std::vector<float> left(256, 0.0f), right(256, 0.0f);
        audio::TransportState t;
        t.playing = true;
        t.sampleRate = 48000.0;
        chain.process(left.data(), right.data(), 256, &t);
        float gain = 1.0f, pan = 0.0f;
        chain.automateMix(t, gain, pan);
        const double expected = route.curve.value(route.curve.position(1.0) - 0.5);
        c.check(near(gain, expected, 1e-4) && pan == 0.0f, "Modulation: a macro moves the track's volume", text(gain));
    }

    return c.failures;
}

int runModulationProjectChecks()
{
    Checker c;
    daw::Project p;
    p.tracks.resize(1);
    daw::Track &t = p.tracks[0];
    t.name = "Pad";
    t.kind = daw::TrackKind::Audio;
    t.effects.push_back(audio::effectSetting(audio::EffectType::Filter));
    audio::Modulator lfo{2, audio::modulatorSetting(audio::ModulatorKind::Lfo)};
    lfo.setting.setValue(1, 3.5);
    audio::Modulator steps{5, audio::modulatorSetting(audio::ModulatorKind::Steps)};
    t.modulators = {lfo, steps};
    t.effects[0].modulation = {{2, 1, -0.4f}};
    t.mixModulation = {{5, audio::kPanParam, 0.75f}};

    // The file keeps modulators, a device's routes and the mix's
    {
        const std::string written = daw::writeProject(p);
        daw::Project back;
        std::string error;
        const bool read = daw::readProject(written, back, error);
        c.check(read && back.tracks[0].modulators == t.modulators && back.tracks[0].effects[0].modulation == t.effects[0].modulation &&
                    back.tracks[0].mixModulation == t.mixModulation,
                "File: modulators and routes round trip", error);
        std::string orphan = written;
        orphan.replace(orphan.find("source = 2"), 10, "source = 9");
        daw::Project dropped;
        c.check(daw::readProject(orphan, dropped, error) && dropped.tracks[0].effects[0].modulation.empty(),
                "File: a route to a modulator the track hasn't is dropped", error);
        std::string twice = written;
        twice.replace(twice.find("number = 5"), 10, "number = 2");
        daw::Project refused;
        std::string why;
        c.check(!daw::readProject(twice, refused, why) && why.find("two modulators") != std::string::npos,
                "File: two modulators with one number are refused", why);
    }

    // The history names modulation's edits; the freeze hash follows the modulators that move devices
    {
        daw::Project none = p;
        none.tracks[0].modulators.pop_back();
        none.tracks[0].mixModulation.clear();
        daw::Project faster = p;
        faster.tracks[0].modulators[0].setting.setValue(1, 7.0);
        daw::Project unrouted = p;
        unrouted.tracks[0].effects[0].modulation.clear();
        daw::Project deeper = p;
        deeper.tracks[0].effects[0].modulation[0].depth = 0.9f;
        c.check(daw::describeChange(none, p) == "Add modulator" && daw::describeChange(p, faster) == "Modulator settings" &&
                    daw::describeChange(unrouted, p) == "Modulate" && daw::describeChange(p, unrouted) == "Remove modulation" &&
                    daw::describeChange(p, deeper) == "Change modulation" && !daw::sameSaved(p, deeper),
                "History: modulation's edits are named",
                daw::describeChange(none, p) + " | " + daw::describeChange(p, faster) + " | " + daw::describeChange(unrouted, p) + " | " +
                    daw::describeChange(p, unrouted) + " | " + daw::describeChange(p, deeper));
        c.check(daw::freezeHash(p, 0) != daw::freezeHash(faster, 0) && daw::freezeHash(p, 0) != daw::freezeHash(deeper, 0),
                "Freeze: the hash follows modulators and their routes");
    }

    return c.failures;
}

}
