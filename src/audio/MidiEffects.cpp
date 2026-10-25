#include "MidiEffects.h"

#include <algorithm>
#include <cmath>

namespace audio {

namespace {

using U = ParamUnit;

// Note lengths as beats, for the synced rates and times
constexpr double kArpRates[] = {1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125};
constexpr double kEchoTimes[] = {1.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 1.0 / 6.0};

const MidiEffectInfo kInfos[kMidiEffectTypeCount] = {
    {"Arpeggiator", "arpeggiator", 8,
     {{{"Pattern", "pattern", 0, 6, 0, U::Choice, false, "Up|Down|Up-down|Down-up|As played|Random|Chord"},
       {"Rate", "rate", 0, 5, 3, U::Choice, false, "1/4|1/8|1/8T|1/16|1/16T|1/32"},
       {"Octaves", "octaves", 1, 4, 1, U::None},
       {"Gate", "gate", 5, 200, 50, U::Percent},
       {"Swing", "swing", 0, 75, 0, U::Percent},
       {"Velocity", "velocityMode", 0, 1, 0, U::Choice, false, "As played|Fixed"},
       {"Level", "level", 1, 127, 100, U::None},
       {"Hold", "hold", 0, 1, 0, U::Toggle}}}},
    {"Chord", "chord", 3,
     {{{"Shape", "shape", 0, 10, 0, U::Choice, false, "Major|Minor|7th|Major 7th|Minor 7th|Sus2|Sus4|Power|Octave|Diminished|Augmented"},
       {"Inversion", "inversion", 0, 3, 0, U::None},
       {"Level", "level", 0, 100, 100, U::Percent}}}},
    {"Scale", "scale", 3,
     {{{"Root", "root", 0, 11, 0, U::Choice, false, "C|C#|D|D#|E|F|F#|G|G#|A|A#|B"},
       {"Scale", "scale", 0, 11, 0, U::Choice, false,
        "Major|Minor|Dorian|Phrygian|Lydian|Mixolydian|Harmonic minor|Melodic minor|Major pentatonic|Minor pentatonic|Blues|Chromatic"},
       {"Direction", "direction", 0, 2, 0, U::Choice, false, "Nearest|Up|Down"}}}},
    {"Echo", "echo", 4,
     {{{"Time", "time", 0, 5, 1, U::Choice, false, "1/4|1/8|1/8D|1/8T|1/16|1/16T"},
       {"Echoes", "echoes", 1, 8, 3, U::None},
       {"Feedback", "feedback", 5, 100, 60, U::Percent},
       {"Pitch", "pitch", -12, 12, 0, U::Semitones}}}},
    {"Velocity", "velocity", 4,
     {{{"Drive", "drive", -100, 100, 0, U::None},
       {"Out low", "low", 1, 127, 1, U::None},
       {"Out high", "high", 1, 127, 127, U::None},
       {"Random", "random", 0, 100, 0, U::Percent}}}},
};

// The scales as twelve-bit masks of their notes from the root (bit 0 the root)
constexpr uint16_t kScales[] = {
    0b101010110101, // major: 0 2 4 5 7 9 11
    0b010110101101, // natural minor: 0 2 3 5 7 8 10
    0b011010101101, // dorian: 0 2 3 5 7 9 10
    0b010110101011, // phrygian: 0 1 3 5 7 8 10
    0b101011010101, // lydian: 0 2 4 6 7 9 11
    0b011010110101, // mixolydian: 0 2 4 5 7 9 10
    0b100110101101, // harmonic minor: 0 2 3 5 7 8 11
    0b101010101101, // melodic minor: 0 2 3 5 7 9 11
    0b001010010101, // major pentatonic: 0 2 4 7 9
    0b010010101001, // minor pentatonic: 0 3 5 7 10
    0b010011101001, // blues: 0 3 5 6 7 10
    0b111111111111, // chromatic
};

// The chord shapes' intervals from the root (up to six; -1 ends a shape)
constexpr int kShapes[][6] = {
    {0, 4, 7, -1}, {0, 3, 7, -1}, {0, 4, 7, 10, -1}, {0, 4, 7, 11, -1}, {0, 3, 7, 10, -1}, {0, 2, 7, -1},
    {0, 5, 7, -1}, {0, 7, -1}, {0, 12, -1}, {0, 3, 6, -1}, {0, 4, 8, -1},
};

int choice(float value, int count)
{
    return std::clamp(static_cast<int>(std::lround(value)), 0, count - 1);
}

// The arpeggiator: steps on the song's grid through the notes held
class Arpeggiator : public MidiEffect {
public:
    Arpeggiator() : MidiEffect(MidiEffectType::Arpeggiator) {}

protected:
    void process(const MidiEventList &in, MidiEventList &out, const MidiClock &clock) override
    {
        const double rate = kArpRates[choice(param(1), 6)];
        const double swing = rate * static_cast<double>(param(4)) / 150.0; // 50% delays the off-beats a third of a step
        // The steps in this block, each played with the held notes as they are at its frame
        size_t next = 0;
        const double b0 = clock.beat0, b1 = clock.beatAt(clock.frames);
        for (int64_t k = static_cast<int64_t>(std::floor(b0 / rate)) - 1;; ++k) {
            const double beat = static_cast<double>(k) * rate + ((k & 1) ? swing : 0.0);
            if (beat >= b1)
                break;
            if (beat < b0)
                continue;
            const auto frame = static_cast<uint32_t>(std::clamp(std::floor(clock.frameAt(beat) + 0.5), 0.0, double(clock.frames - 1)));
            for (; next < in.count && in.events[next].frame <= frame; ++next)
                take(in.events[next]);
            step(out, clock, frame, k, rate);
        }
        for (; next < in.count; ++next)
            take(in.events[next]);
        due(out, clock); // the note-offs falling in this block, those just made included
    }

    void release(MidiEventList &out, uint32_t frame) override
    {
        dropPending();
        releaseCounted(out, frame);
        count_ = 0;
        down_ = 0;
        index_ = 0;
    }

private:
    struct Held {
        int key;
        float velocity;
    };

    // A key pressed or let go
    void take(const MidiEvent &e)
    {
        const bool hold = param(7) >= 0.5f;
        if (e.on) {
            // With Hold, a key pressed after every key was let go starts a new chord
            if (hold && down_ == 0)
                count_ = 0;
            if (count_ == 0)
                index_ = 0;
            ++down_;
            for (size_t i = 0; i < count_; ++i)
                if (held_[i].key == e.key)
                    return;
            if (count_ < held_.size())
                held_[count_++] = {e.key, e.velocity};
            return;
        }
        down_ = down_ > 0 ? down_ - 1 : 0;
        if (hold)
            return;
        for (size_t i = 0; i < count_; ++i) {
            if (held_[i].key == e.key) {
                for (size_t j = i + 1; j < count_; ++j)
                    held_[j - 1] = held_[j];
                --count_;
                break;
            }
        }
    }

    // One step: the pattern's next note (or the chord), and its note-off after the gate
    void step(MidiEventList &out, const MidiClock &clock, uint32_t frame, int64_t k, double rate)
    {
        if (count_ == 0)
            return;
        const int pattern = choice(param(0), 7);
        const int octaves = std::clamp(static_cast<int>(std::lround(param(2))), 1, 4);
        const bool fixed = param(5) >= 0.5f;
        const float level = std::clamp(param(6), 1.0f, 127.0f) / 127.0f;
        // The notes in order: by pitch, or as they were played
        std::array<Held, 32> sorted{};
        std::copy_n(held_.begin(), count_, sorted.begin());
        if (pattern != 4)
            std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count_),
                      [](const Held &a, const Held &b) { return a.key < b.key; });
        const auto n = static_cast<int64_t>(count_) * octaves;
        auto noteAt = [&](int64_t i) {
            const Held &h = sorted[static_cast<size_t>(i % static_cast<int64_t>(count_))];
            return Held{h.key + 12 * static_cast<int>(i / static_cast<int64_t>(count_)), h.velocity};
        };
        const auto length = static_cast<uint64_t>(std::max(1.0, clock.framesFor(rate * static_cast<double>(param(3)) / 100.0)));
        const uint64_t at = clock.start + frame;
        auto play = [&](Held h) {
            if (h.key < 0 || h.key > 127)
                return;
            const float velocity = fixed ? level : h.velocity;
            noteOn(out, frame, h.key, velocity);
            schedule(at + length, false, h.key, 0.0f);
        };
        if (pattern == 6) {
            const int octave = static_cast<int>(index_ % octaves);
            for (size_t i = 0; i < count_; ++i)
                play({sorted[i].key + 12 * octave, sorted[i].velocity});
        } else {
            int64_t i = 0;
            const int64_t cycle = std::max<int64_t>(1, 2 * n - 2);
            switch (pattern) {
            case 1: i = n - 1 - index_ % n; break;
            case 2: {
                const int64_t p = index_ % cycle;
                i = n == 1 ? 0 : p < n ? p : cycle - p;
                break;
            }
            case 3: {
                const int64_t p = index_ % cycle;
                i = n == 1 ? 0 : p < n ? n - 1 - p : p - n + 1;
                break;
            }
            case 5: i = static_cast<int64_t>(midiHash(static_cast<uint64_t>(k), 77) * static_cast<float>(n)) % n; break;
            default: i = index_ % n; break; // up, as played
            }
            play(noteAt(i));
        }
        ++index_;
    }

    std::array<Held, 32> held_{};
    size_t count_ = 0;   // notes in the pattern
    int down_ = 0;       // keys down now
    int64_t index_ = 0;  // steps since the pattern started
};

class Chord : public MidiEffect {
public:
    Chord() : MidiEffect(MidiEffectType::Chord) { made_.fill({}); }

protected:
    void process(const MidiEventList &in, MidiEventList &out, const MidiClock &) override
    {
        for (size_t i = 0; i < in.count; ++i) {
            const MidiEvent &e = in.events[i];
            if (e.key < 0 || e.key > 127)
                continue;
            Made &m = made_[static_cast<size_t>(e.key)];
            if (!e.on) {
                for (int j = 0; j < m.count; ++j)
                    noteOff(out, e.frame, m.keys[static_cast<size_t>(j)]);
                m.count = 0;
                continue;
            }
            // Its shape, inverted: the lowest notes moved up an octave
            const int *shape = kShapes[choice(param(0), 11)];
            std::array<int, 6> keys{};
            int n = 0;
            for (int j = 0; j < 6 && shape[j] >= 0; ++j)
                keys[static_cast<size_t>(n++)] = e.key + shape[j];
            const int inversion = std::clamp(static_cast<int>(std::lround(param(1))), 0, n - 1);
            for (int j = 0; j < inversion; ++j)
                keys[static_cast<size_t>(j)] += 12;
            const float level = std::clamp(param(2), 0.0f, 100.0f) / 100.0f;
            for (int j = 0; j < m.count; ++j) // the same key again: its last chord ends first
                noteOff(out, e.frame, m.keys[static_cast<size_t>(j)]);
            m.count = 0;
            for (int j = 0; j < n; ++j) {
                const int key = keys[static_cast<size_t>(j)];
                if (key < 0 || key > 127)
                    continue;
                noteOn(out, e.frame, key, key == e.key ? e.velocity : e.velocity * level);
                m.keys[static_cast<size_t>(m.count++)] = key;
            }
        }
    }

    void release(MidiEventList &out, uint32_t frame) override
    {
        releaseCounted(out, frame);
        made_.fill({});
    }

private:
    struct Made {
        std::array<int, 6> keys{};
        int count = 0;
    };
    std::array<Made, 128> made_; // what each key played became
};

class Scale : public MidiEffect {
public:
    Scale() : MidiEffect(MidiEffectType::Scale) { mapped_.fill(-1); }

protected:
    void process(const MidiEventList &in, MidiEventList &out, const MidiClock &) override
    {
        const int root = choice(param(0), 12);
        const uint16_t mask = kScales[choice(param(1), 12)];
        const int direction = choice(param(2), 3);
        auto inScale = [&](int key) { return (mask >> (((key - root) % 12 + 12) % 12)) & 1; };
        for (size_t i = 0; i < in.count; ++i) {
            const MidiEvent &e = in.events[i];
            if (e.key < 0 || e.key > 127)
                continue;
            if (!e.on) {
                const int was = mapped_[static_cast<size_t>(e.key)];
                if (was >= 0)
                    noteOff(out, e.frame, was);
                mapped_[static_cast<size_t>(e.key)] = -1;
                continue;
            }
            int key = e.key;
            for (int d = 0; d <= 6 && !inScale(key); ++d) {
                // Nearest: up first, then down, a step further each time
                if (direction == 1) {
                    key = e.key + d + 1;
                } else if (direction == 2) {
                    key = e.key - d - 1;
                } else {
                    if (inScale(e.key + d + 1)) {
                        key = e.key + d + 1;
                        break;
                    }
                    key = e.key - d - 1;
                }
            }
            key = std::clamp(key, 0, 127);
            if (mapped_[static_cast<size_t>(e.key)] >= 0)
                noteOff(out, e.frame, mapped_[static_cast<size_t>(e.key)]);
            mapped_[static_cast<size_t>(e.key)] = key;
            noteOn(out, e.frame, key, e.velocity);
        }
    }

    void release(MidiEventList &out, uint32_t frame) override
    {
        releaseCounted(out, frame);
        mapped_.fill(-1);
    }

private:
    std::array<int, 128> mapped_{}; // what each key played became (-1: not sounding)
};

class Echo : public MidiEffect {
public:
    Echo() : MidiEffect(MidiEffectType::Echo) {}

protected:
    void process(const MidiEventList &in, MidiEventList &out, const MidiClock &clock) override
    {
        const double time = clock.framesFor(kEchoTimes[choice(param(0), 6)]);
        const int echoes = std::clamp(static_cast<int>(std::lround(param(1))), 1, 8);
        const float feedback = std::clamp(param(2), 0.0f, 100.0f) / 100.0f;
        const int pitch = static_cast<int>(std::lround(param(3)));
        // Each note goes through, and each echo waits its time (its note-off as far after it)
        for (size_t i = 0; i < in.count; ++i) {
            const MidiEvent &e = in.events[i];
            if (e.on)
                noteOn(out, e.frame, e.key, e.velocity);
            else
                noteOff(out, e.frame, e.key);
            float velocity = e.velocity;
            for (int n = 1; n <= echoes; ++n) {
                velocity *= feedback;
                const int key = e.key + pitch * n;
                if (key < 0 || key > 127)
                    continue;
                schedule(clock.start + e.frame + static_cast<uint64_t>(time * n), e.on, key, velocity);
            }
        }
        due(out, clock);
    }

    void release(MidiEventList &out, uint32_t frame) override
    {
        dropPending();
        releaseCounted(out, frame);
    }
};

class Velocity : public MidiEffect {
public:
    Velocity() : MidiEffect(MidiEffectType::Velocity) {}

protected:
    void process(const MidiEventList &in, MidiEventList &out, const MidiClock &clock) override
    {
        const double drive = std::clamp(param(0), -100.0f, 100.0f);
        const double exponent = std::pow(2.0, -drive / 50.0); // above 0 lifts soft notes, below pushes them down
        const float low = std::clamp(param(1), 1.0f, 127.0f) / 127.0f;
        const float high = std::max(low, std::clamp(param(2), 1.0f, 127.0f) / 127.0f);
        const float random = std::clamp(param(3), 0.0f, 100.0f) / 100.0f;
        for (size_t i = 0; i < in.count; ++i) {
            MidiEvent e = in.events[i];
            if (e.on) {
                float v = static_cast<float>(std::pow(std::clamp(static_cast<double>(e.velocity), 0.0, 1.0), exponent));
                v = low + (high - low) * v;
                if (random > 0.0f)
                    v += (midiHash(clock.start + e.frame, static_cast<uint64_t>(e.key)) - 0.5f) * random;
                e.velocity = std::clamp(v, 1.0f / 127.0f, 1.0f);
            }
            out.push(e);
        }
    }

    void release(MidiEventList &, uint32_t) override {}
};

}

const MidiEffectInfo &midiEffectInfo(MidiEffectType type)
{
    return kInfos[static_cast<int>(type)];
}

bool midiEffectFromKey(const std::string &key, MidiEffectType &type)
{
    for (int i = 0; i < kMidiEffectTypeCount; ++i) {
        if (key == kInfos[i].key) {
            type = static_cast<MidiEffectType>(i);
            return true;
        }
    }
    return false;
}

DeviceSetting midiEffectSetting(MidiEffectType type)
{
    const MidiEffectInfo &info = midiEffectInfo(type);
    DeviceSetting s;
    s.kind = DeviceKind::MidiEffect;
    s.id = info.key;
    s.name = info.name;
    for (size_t i = 0; i < info.paramCount; ++i)
        s.setValue(static_cast<uint32_t>(i), info.params[i].def);
    return s;
}

void MidiEventList::sort()
{
    std::stable_sort(events.begin(), events.begin() + static_cast<std::ptrdiff_t>(count), [](const MidiEvent &a, const MidiEvent &b) {
        if (a.frame != b.frame)
            return a.frame < b.frame;
        return !a.on && b.on;
    });
}

MidiEffect::MidiEffect(MidiEffectType type) : type_(type)
{
    const MidiEffectInfo &info = midiEffectInfo(type);
    for (size_t i = 0; i < info.paramCount; ++i)
        values_[i].store(info.params[i].def, std::memory_order_relaxed);
}

bool MidiEffect::matches(const DeviceSetting &setting) const
{
    MidiEffectType type;
    return setting.kind == DeviceKind::MidiEffect && midiEffectFromKey(setting.id, type) && type == type_;
}

void MidiEffect::applySetting(const DeviceSetting &setting)
{
    const MidiEffectInfo &info = midiEffectInfo(type_);
    for (size_t i = 0; i < info.paramCount; ++i)
        setParam(i, static_cast<float>(setting.value(static_cast<uint32_t>(i), info.params[i].def)));
    setBypass(setting.bypass);
}

std::vector<DeviceParam> MidiEffect::params() const
{
    const MidiEffectInfo &info = midiEffectInfo(type_);
    std::vector<DeviceParam> out;
    for (size_t i = 0; i < info.paramCount; ++i) {
        DeviceParam p = effectParam(info.params[i], static_cast<uint32_t>(i));
        // Counts are whole numbers
        p.stepped = p.stepped || (info.params[i].unit == ParamUnit::None && info.params[i].max - info.params[i].min <= 200);
        out.push_back(p);
    }
    return out;
}

void MidiEffect::setParam(size_t index, float value)
{
    const MidiEffectInfo &info = midiEffectInfo(type_);
    if (index >= info.paramCount)
        return;
    values_[index].store(std::clamp(value, info.params[index].min, info.params[index].max), std::memory_order_relaxed);
}

void MidiEffect::run(const MidiEventList &in, MidiEventList &out, const MidiClock &clock)
{
    const bool bypassed = this->bypassed();
    if (bypassed && !wasBypassed_)
        release(out, 0); // what it made ends as it steps aside
    wasBypassed_ = bypassed;
    if (bypassed) {
        for (size_t i = 0; i < in.count; ++i)
            out.push(in.events[i]);
        return;
    }
    process(in, out, clock);
}

void MidiEffect::reset()
{
    MidiEventList ignored;
    release(ignored, 0);
    dropPending();
    counts_.fill(0);
}

void MidiEffect::schedule(uint64_t at, bool on, int key, float velocity)
{
    if (pendingCount_ < kMaxPending)
        pending_[pendingCount_++] = {at, on, key, velocity};
}

void MidiEffect::due(MidiEventList &out, const MidiClock &clock)
{
    const uint64_t end = clock.start + clock.frames;
    for (size_t i = 0; i < pendingCount_;) {
        const Pending &p = pending_[i];
        if (p.at < end) {
            const auto frame = static_cast<uint32_t>(p.at > clock.start ? p.at - clock.start : 0);
            if (p.on)
                noteOn(out, frame, p.key, p.velocity);
            else
                noteOff(out, frame, p.key);
            pending_[i] = pending_[--pendingCount_];
        } else {
            ++i;
        }
    }
}

void MidiEffect::noteOn(MidiEventList &out, uint32_t frame, int key, float velocity)
{
    if (key < 0 || key > 127)
        return;
    uint8_t &c = counts_[static_cast<size_t>(key)];
    c = static_cast<uint8_t>(std::min(255, c + 1));
    out.push({frame, true, key, velocity});
}

void MidiEffect::noteOff(MidiEventList &out, uint32_t frame, int key)
{
    if (key < 0 || key > 127)
        return;
    uint8_t &c = counts_[static_cast<size_t>(key)];
    if (c == 0)
        return;
    if (--c == 0)
        out.push({frame, false, key, 0.0f});
}

void MidiEffect::releaseCounted(MidiEventList &out, uint32_t frame)
{
    for (size_t k = 0; k < 128; ++k) {
        if (counts_[k] > 0)
            out.push({frame, false, static_cast<int>(k), 0.0f});
        counts_[k] = 0;
    }
}

std::unique_ptr<MidiEffect> makeMidiEffect(const DeviceSetting &setting)
{
    MidiEffectType type = MidiEffectType::Velocity;
    midiEffectFromKey(setting.id, type);
    std::unique_ptr<MidiEffect> effect;
    switch (type) {
    case MidiEffectType::Arpeggiator: effect = std::make_unique<Arpeggiator>(); break;
    case MidiEffectType::Chord: effect = std::make_unique<Chord>(); break;
    case MidiEffectType::Scale: effect = std::make_unique<Scale>(); break;
    case MidiEffectType::Echo: effect = std::make_unique<Echo>(); break;
    case MidiEffectType::Velocity: effect = std::make_unique<Velocity>(); break;
    }
    effect->applySetting(setting);
    return effect;
}

float midiHash(uint64_t a, uint64_t b)
{
    uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<float>(x >> 40) / 16777216.0f;
}

}
