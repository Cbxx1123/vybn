#include "NoteTools.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <numeric>

namespace daw {

namespace {

constexpr double kMinLength = 1.0 / 64.0;

// The notes a tool works on: the selected ones, or every one when none are
std::vector<size_t> targets(const std::vector<Note> &notes)
{
    std::vector<size_t> out;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].selected)
            out.push_back(i);
    }
    if (out.empty()) {
        out.resize(notes.size());
        std::iota(out.begin(), out.end(), size_t{0});
    }
    return out;
}

// A small deterministic random source (xorshift), so tools and tests repeat
struct Random {
    uint32_t state;
    explicit Random(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}
    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    double unit() { return (next() & 0xFFFFFF) / static_cast<double>(0x1000000); } // [0, 1)
    double signedUnit() { return unit() * 2.0 - 1.0; }
};

int clampKey(int key) { return std::clamp(key, 0, 127); }
float clampVelocity(float v) { return std::clamp(v, 1.0f / 127.0f, 1.0f); }

int mod12(int v) { return ((v % 12) + 12) % 12; }

}

const std::vector<Scale> &scales()
{
    static const std::vector<Scale> all{
        {"Major", {0, 2, 4, 5, 7, 9, 11}},
        {"Natural minor", {0, 2, 3, 5, 7, 8, 10}},
        {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}},
        {"Melodic minor", {0, 2, 3, 5, 7, 9, 11}},
        {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"Lydian", {0, 2, 4, 6, 7, 9, 11}},
        {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"Locrian", {0, 1, 3, 5, 6, 8, 10}},
        {"Major pentatonic", {0, 2, 4, 7, 9}},
        {"Minor pentatonic", {0, 3, 5, 7, 10}},
        {"Blues", {0, 3, 5, 6, 7, 10}},
        {"Whole tone", {0, 2, 4, 6, 8, 10}},
        {"Chromatic", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    };
    return all;
}

const Scale *findScale(const std::string &name)
{
    for (const Scale &s : scales()) {
        if (s.name == name)
            return &s;
    }
    return nullptr;
}

bool inScale(int key, int root, const Scale &scale) { return scaleDegree(key, root, scale) >= 0; }

int scaleDegree(int key, int root, const Scale &scale)
{
    const int pc = mod12(key - root);
    for (size_t i = 0; i < scale.steps.size(); ++i) {
        if (scale.steps[i] == pc)
            return static_cast<int>(i);
    }
    return -1;
}

int snapToScale(int key, int root, const Scale &scale)
{
    for (int d = 0; d <= 6; ++d) {
        if (key - d >= 0 && inScale(key - d, root, scale))
            return key - d;
        if (key + d <= 127 && inScale(key + d, root, scale))
            return key + d;
    }
    return clampKey(key);
}

int stepInScale(int key, int degrees, int root, const Scale &scale)
{
    int k = snapToScale(key, root, scale);
    const int dir = degrees >= 0 ? 1 : -1;
    for (int n = 0; n != degrees; n += dir) {
        int next = k + dir;
        while (next >= 0 && next <= 127 && !inScale(next, root, scale))
            next += dir;
        if (next < 0 || next > 127)
            break;
        k = next;
    }
    return k;
}

const char *rootName(int root)
{
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[mod12(root)];
}

const std::vector<ChordShape> &chordShapes()
{
    static const std::vector<ChordShape> all{
        {"Diatonic triad", {}, 3},
        {"Diatonic seventh", {}, 4},
        {"Major", {0, 4, 7}, 0},
        {"Minor", {0, 3, 7}, 0},
        {"Dominant 7th", {0, 4, 7, 10}, 0},
        {"Major 7th", {0, 4, 7, 11}, 0},
        {"Minor 7th", {0, 3, 7, 10}, 0},
        {"Sus2", {0, 2, 7}, 0},
        {"Sus4", {0, 5, 7}, 0},
        {"Diminished", {0, 3, 6}, 0},
        {"Augmented", {0, 4, 8}, 0},
        {"Add9", {0, 4, 7, 14}, 0},
        {"Power", {0, 7, 12}, 0},
        {"Octave", {0, 12}, 0},
    };
    return all;
}

std::vector<int> chordKeys(const ChordShape &shape, int root, int scaleRoot, const Scale &scale)
{
    std::vector<int> keys;
    if (shape.diatonicNotes > 0) {
        const int base = snapToScale(root, scaleRoot, scale);
        for (int i = 0; i < shape.diatonicNotes; ++i)
            keys.push_back(stepInScale(base, i * 2, scaleRoot, scale));
    } else {
        for (int interval : shape.intervals)
            keys.push_back(clampKey(root + interval));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::vector<Note> scaleRun(int fromKey, int octaves, int direction, double start, double step, float velocity,
                           int scaleRoot, const Scale &scale)
{
    std::vector<int> keys;
    const int count = static_cast<int>(scale.steps.size()) * std::max(1, octaves);
    const int first = snapToScale(fromKey, scaleRoot, scale);
    if (direction >= 0) {
        for (int i = 0; i <= count; ++i)
            keys.push_back(stepInScale(first, i, scaleRoot, scale));
        if (direction == 0) {
            for (int i = count - 1; i >= 0; --i)
                keys.push_back(stepInScale(first, i, scaleRoot, scale));
        }
    } else {
        for (int i = 0; i <= count; ++i)
            keys.push_back(stepInScale(first, -i, scaleRoot, scale));
    }
    std::vector<Note> out;
    for (size_t i = 0; i < keys.size(); ++i) {
        Note n;
        n.start = start + static_cast<double>(i) * step;
        n.length = step;
        n.key = keys[i];
        n.velocity = clampVelocity(velocity);
        n.selected = true;
        out.push_back(n);
    }
    return out;
}

std::vector<Note> chord(const std::vector<int> &keys, double start, double length, float velocity)
{
    std::vector<Note> out;
    for (int k : keys) {
        Note n;
        n.start = start;
        n.length = length;
        n.key = clampKey(k);
        n.velocity = clampVelocity(velocity);
        n.selected = true;
        out.push_back(n);
    }
    return out;
}

const std::vector<ArpPattern> &arpPatterns()
{
    static const std::vector<ArpPattern> all{ArpPattern::Up,       ArpPattern::Down,   ArpPattern::UpDown,     ArpPattern::UpAndDown,
                                             ArpPattern::DownUp,   ArpPattern::Converge, ArpPattern::Diverge,  ArpPattern::Thumb,
                                             ArpPattern::Pinky,    ArpPattern::Random, ArpPattern::RandomWalk, ArpPattern::AsPlayed,
                                             ArpPattern::Chord};
    return all;
}

const char *arpPatternName(ArpPattern pattern)
{
    switch (pattern) {
    case ArpPattern::Up: return "Up";
    case ArpPattern::Down: return "Down";
    case ArpPattern::UpDown: return "Up and down";
    case ArpPattern::UpAndDown: return "Up and down, ends twice";
    case ArpPattern::DownUp: return "Down and up";
    case ArpPattern::Converge: return "Converge";
    case ArpPattern::Diverge: return "Diverge";
    case ArpPattern::Thumb: return "Thumb (low between)";
    case ArpPattern::Pinky: return "Pinky (high between)";
    case ArpPattern::Random: return "Random";
    case ArpPattern::RandomWalk: return "Random walk";
    case ArpPattern::AsPlayed: return "As played";
    case ArpPattern::Chord: return "Chord";
    }
    return "";
}

const std::vector<ArpPreset> &arpPresets()
{
    static const std::vector<ArpPreset> presets = [] {
        std::vector<ArpPreset> p;
        auto add = [&p](const char *name, auto change) {
            ArpSettings s;
            change(s);
            p.push_back({name, s});
        };
        add("Classic up, 1/16", [](ArpSettings &) {});
        add("Up and down, two octaves", [](ArpSettings &s) {
            s.pattern = ArpPattern::UpDown;
            s.octaves = 2;
        });
        add("Alberti bass, 1/8", [](ArpSettings &s) {
            s.pattern = ArpPattern::Pinky;
            s.rate = 0.5;
            s.gate = 0.9;
        });
        add("Harp sweep", [](ArpSettings &s) {
            s.pattern = ArpPattern::UpAndDown;
            s.rate = 0.125;
            s.octaves = 3;
            s.gate = 1.6;
            s.velocityMode = ArpVelocity::Rising;
        });
        add("Trance gate", [](ArpSettings &s) {
            s.pattern = ArpPattern::Chord;
            s.rhythm = "X.xx.xX.x.xx.xx.";
            s.gate = 0.5;
        });
        add("Swung eighths", [](ArpSettings &s) {
            s.pattern = ArpPattern::UpDown;
            s.rate = 0.5;
            s.swing = 0.6;
        });
        add("Ratchet runs", [](ArpSettings &s) {
            s.rate = 0.5;
            s.repeats = 2;
            s.rhythm = "x-xX";
            s.gate = 0.6;
        });
        add("Climbing thirds", [](ArpSettings &s) {
            s.pattern = ArpPattern::Converge;
            s.transpose = 2;
            s.octaves = 2;
        });
        add("Random walk, humanized", [](ArpSettings &s) {
            s.pattern = ArpPattern::RandomWalk;
            s.octaves = 2;
            s.humanize = 0.5;
        });
        add("Broken chord, triplets", [](ArpSettings &s) {
            s.pattern = ArpPattern::Thumb;
            s.rate = 1.0 / 3.0;
        });
        return p;
    }();
    return presets;
}

int arpeggiate(std::vector<Note> &notes, const ArpSettings &settings)
{
    std::vector<size_t> chosen = targets(notes);
    if (chosen.empty() || settings.rate <= 0.0)
        return 0;
    std::stable_sort(chosen.begin(), chosen.end(), [&](size_t a, size_t b) { return notes[a].start < notes[b].start; });

    // Group notes that overlap in time into chords
    struct Group {
        double start, end;
        std::vector<const Note *> members;
    };
    std::vector<Group> groups;
    for (size_t i : chosen) {
        const Note &n = notes[i];
        if (!groups.empty() && n.start < groups.back().end - 1e-9) {
            groups.back().end = std::max(groups.back().end, n.start + n.length);
            groups.back().members.push_back(&n);
        } else {
            groups.push_back({n.start, n.start + n.length, {&n}});
        }
    }

    // The rhythm's steps (anything but the four step marks is left out)
    std::string rhythm;
    for (char ch : settings.rhythm) {
        if (ch == 'x' || ch == 'X' || ch == '-' || ch == '.')
            rhythm += ch;
    }
    if (rhythm.empty())
        rhythm = "x";

    Random random(settings.seed);
    auto unit = [&random] { return static_cast<double>(random.next() % 100001u) / 100000.0; }; // 0-1
    const int octaves = std::clamp(settings.octaves, 1, 4);
    const int repeats = std::clamp(settings.repeats, 1, 4);
    const double gate = std::clamp(settings.gate, 0.05, 2.0);
    const double swing = std::clamp(settings.swing, 0.0, 1.0) * settings.rate / 3.0;
    const double humanize = std::clamp(settings.humanize, 0.0, 1.0);
    std::vector<Note> made;
    for (const Group &g : groups) {
        // The keys to step through, low to high (or as played), across the octaves
        std::vector<const Note *> played = g.members;
        if (settings.pattern != ArpPattern::AsPlayed)
            std::stable_sort(played.begin(), played.end(), [](const Note *a, const Note *b) { return a->key < b->key; });
        std::vector<std::pair<int, float>> tones; // key, velocity
        for (int o = 0; o < octaves; ++o) {
            const int shift = settings.octaveMode == ArpOctaves::Up     ? 12 * o
                              : settings.octaveMode == ArpOctaves::Down ? -12 * (octaves - 1 - o)
                                                                        : 12 * (o - (octaves - 1) / 2);
            for (const Note *n : played) {
                const int k = n->key + shift;
                if (k >= 0 && k <= 127)
                    tones.push_back({k, n->velocity});
            }
        }
        if (tones.empty())
            continue;

        // The order the pattern goes through them (Random and Random walk choose as they go)
        std::vector<std::pair<int, float>> order;
        const size_t count = tones.size();
        switch (settings.pattern) {
        case ArpPattern::Down:
            order.assign(tones.rbegin(), tones.rend());
            break;
        case ArpPattern::UpDown:
            order = tones;
            for (size_t i = count >= 2 ? count - 2 : 0; i >= 1 && i < count; --i)
                order.push_back(tones[i]);
            break;
        case ArpPattern::UpAndDown:
            order = tones;
            order.insert(order.end(), tones.rbegin(), tones.rend());
            break;
        case ArpPattern::DownUp:
            order.assign(tones.rbegin(), tones.rend());
            for (size_t i = 1; i + 1 < count; ++i)
                order.push_back(tones[i]);
            break;
        case ArpPattern::Converge:
        case ArpPattern::Diverge: {
            for (size_t lo = 0, hi = count - 1; lo <= hi && hi < count; ++lo, --hi) {
                order.push_back(tones[lo]);
                if (hi != lo)
                    order.push_back(tones[hi]);
                if (hi == 0)
                    break;
            }
            if (settings.pattern == ArpPattern::Diverge)
                std::reverse(order.begin(), order.end());
            break;
        }
        case ArpPattern::Thumb:
            for (size_t i = 1; i < count; ++i) {
                order.push_back(tones.front());
                order.push_back(tones[i]);
            }
            break;
        case ArpPattern::Pinky:
            for (size_t i = 0; i + 1 < count; ++i) {
                order.push_back(tones[i]);
                order.push_back(tones.back());
            }
            break;
        default:
            order = tones;
            break;
        }
        if (order.empty())
            order = tones;

        const double end = settings.length > 0.0 ? g.start + settings.length : g.end;
        const int steps = static_cast<int>(std::ceil((end - g.start) / settings.rate - 1e-9));
        size_t next = 0;       // the pattern's place: how many steps have played
        size_t walk = 0;       // Random walk's place
        size_t lastPick = count;
        size_t firstOfStep = made.size(), lastStepNotes = 0;
        for (int step = 0; step < steps; ++step) {
            const char mark = rhythm[static_cast<size_t>(step) % rhythm.size()];
            if (mark == '.')
                continue;
            if (mark == '-') {
                // Ties: the notes of the step before last another step
                for (size_t i = firstOfStep; i < firstOfStep + lastStepNotes && i < made.size(); ++i)
                    made[i].length = std::min(made[i].length + settings.rate, end - made[i].start);
                continue;
            }
            // This step's keys
            std::vector<std::pair<int, float>> keys;
            if (settings.pattern == ArpPattern::Chord) {
                keys = tones;
            } else if (settings.pattern == ArpPattern::Random) {
                size_t pick = random.next() % count;
                if (count > 1 && pick == lastPick)
                    pick = (pick + 1 + random.next() % (count - 1)) % count;
                lastPick = pick;
                keys.push_back(tones[pick]);
            } else if (settings.pattern == ArpPattern::RandomWalk) {
                if (next > 0 && count > 1) {
                    const bool up = walk == 0 || (walk + 1 < count && random.next() % 2 == 0);
                    walk = up ? walk + 1 : walk - 1;
                }
                keys.push_back(tones[walk]);
            } else {
                keys.push_back(order[next % order.size()]);
            }
            const int round = static_cast<int>(next / order.size());
            ++next;

            double start = g.start + step * settings.rate + (step % 2 == 1 ? swing : 0.0);
            if (humanize > 0.0)
                start += (unit() * 2.0 - 1.0) * humanize * settings.rate * 0.1;
            start = std::max(g.start, start);
            const double piece = settings.rate / repeats;
            firstOfStep = made.size();
            lastStepNotes = 0;
            for (int r = 0; r < repeats; ++r) {
                const double at = start + r * piece;
                if (at >= end - 1e-9)
                    break;
                for (const auto &tone : keys) {
                    const int key = tone.first + settings.transpose * round;
                    if (key < 0 || key > 127)
                        continue;
                    float velocity = tone.second;
                    const double progress = steps > 1 ? static_cast<double>(step) / (steps - 1) : 1.0;
                    switch (settings.velocityMode) {
                    case ArpVelocity::Fixed: velocity = settings.velocity; break;
                    case ArpVelocity::Rising: velocity = settings.velocity * static_cast<float>(0.35 + 0.65 * progress); break;
                    case ArpVelocity::Falling: velocity = settings.velocity * static_cast<float>(1.0 - 0.65 * progress); break;
                    case ArpVelocity::AsPlayed: break;
                    }
                    if (mark == 'X')
                        velocity += settings.accent;
                    if (humanize > 0.0)
                        velocity += static_cast<float>((unit() * 2.0 - 1.0) * humanize * 0.12);
                    Note n;
                    n.start = at;
                    n.length = std::max(kMinLength, std::min(piece * gate, end - at));
                    n.key = key;
                    n.velocity = std::clamp(velocity, 1.0f / 127.0f, 1.0f);
                    n.selected = true;
                    made.push_back(n);
                    ++lastStepNotes;
                }
            }
        }
    }

    // Replace the originals
    std::vector<bool> drop(notes.size(), false);
    for (size_t i : chosen)
        drop[i] = true;
    std::vector<Note> kept;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (!drop[i]) {
            kept.push_back(notes[i]);
            kept.back().selected = false;
        }
    }
    kept.insert(kept.end(), made.begin(), made.end());
    notes = std::move(kept);
    return static_cast<int>(made.size());
}

int quantize(std::vector<Note> &notes, double grid, double strength, bool ends)
{
    if (grid <= 0.0)
        return 0;
    strength = std::clamp(strength, 0.0, 1.0);
    int changed = 0;
    for (size_t i : targets(notes)) {
        Note &n = notes[i];
        const double end = n.start + n.length;
        const double start = n.start + (std::round(n.start / grid) * grid - n.start) * strength;
        double newEnd = end + (start - n.start); // keeps the length
        if (ends) {
            const double target = std::max(start + grid, std::round(end / grid) * grid);
            newEnd = end + (target - end) * strength;
        }
        if (std::fabs(start - n.start) > 1e-9 || std::fabs(newEnd - end) > 1e-9)
            ++changed;
        n.start = std::max(0.0, start);
        n.length = std::max(kMinLength, newEnd - n.start);
    }
    return changed;
}

int legato(std::vector<Note> &notes)
{
    std::vector<size_t> chosen = targets(notes);
    std::sort(chosen.begin(), chosen.end(), [&](size_t a, size_t b) { return notes[a].start < notes[b].start; });
    int changed = 0;
    for (size_t j = 0; j < chosen.size(); ++j) {
        Note &n = notes[chosen[j]];
        // The next start after this one (chords keep their notes together)
        double next = -1.0;
        for (size_t k = j + 1; k < chosen.size(); ++k) {
            if (notes[chosen[k]].start > n.start + 1e-9) {
                next = notes[chosen[k]].start;
                break;
            }
        }
        if (next > n.start && std::fabs(n.start + n.length - next) > 1e-9) {
            n.length = next - n.start;
            ++changed;
        }
    }
    return changed;
}

int humanize(std::vector<Note> &notes, double timing, float velocity, uint32_t seed)
{
    Random random(seed);
    int changed = 0;
    for (size_t i : targets(notes)) {
        Note &n = notes[i];
        n.start = std::max(0.0, n.start + random.signedUnit() * timing);
        n.velocity = clampVelocity(n.velocity + static_cast<float>(random.signedUnit()) * velocity);
        ++changed;
    }
    return changed;
}

int setVelocity(std::vector<Note> &notes, float velocity)
{
    int changed = 0;
    for (size_t i : targets(notes)) {
        notes[i].velocity = clampVelocity(velocity);
        ++changed;
    }
    return changed;
}

int scaleVelocity(std::vector<Note> &notes, float factor)
{
    const std::vector<size_t> chosen = targets(notes);
    if (chosen.empty())
        return 0;
    float mean = 0.0f;
    for (size_t i : chosen)
        mean += notes[i].velocity;
    mean /= static_cast<float>(chosen.size());
    for (size_t i : chosen)
        notes[i].velocity = clampVelocity(mean + (notes[i].velocity - mean) * factor);
    return static_cast<int>(chosen.size());
}

int shiftVelocity(std::vector<Note> &notes, float by)
{
    int changed = 0;
    for (size_t i : targets(notes)) {
        notes[i].velocity = clampVelocity(notes[i].velocity + by);
        ++changed;
    }
    return changed;
}

int rampVelocity(std::vector<Note> &notes, float from, float to)
{
    std::vector<size_t> chosen = targets(notes);
    if (chosen.empty())
        return 0;
    double first = 1e30, last = -1e30;
    for (size_t i : chosen) {
        first = std::min(first, notes[i].start);
        last = std::max(last, notes[i].start);
    }
    for (size_t i : chosen) {
        const double t = last > first ? (notes[i].start - first) / (last - first) : 0.0;
        notes[i].velocity = clampVelocity(from + (to - from) * static_cast<float>(t));
    }
    return static_cast<int>(chosen.size());
}

int transpose(std::vector<Note> &notes, int semitones)
{
    int changed = 0;
    for (size_t i : targets(notes)) {
        const int k = clampKey(notes[i].key + semitones);
        if (k != notes[i].key)
            ++changed;
        notes[i].key = k;
    }
    return changed;
}

int fitToScale(std::vector<Note> &notes, int root, const Scale &scale)
{
    int changed = 0;
    for (size_t i : targets(notes)) {
        const int k = snapToScale(notes[i].key, root, scale);
        if (k != notes[i].key)
            ++changed;
        notes[i].key = k;
    }
    return changed;
}

int stretch(std::vector<Note> &notes, double factor)
{
    const std::vector<size_t> chosen = targets(notes);
    if (chosen.empty() || factor <= 0.0)
        return 0;
    double first = 1e30;
    for (size_t i : chosen)
        first = std::min(first, notes[i].start);
    for (size_t i : chosen) {
        Note &n = notes[i];
        n.start = first + (n.start - first) * factor;
        n.length = std::max(kMinLength, n.length * factor);
    }
    return static_cast<int>(chosen.size());
}

int reverse(std::vector<Note> &notes)
{
    const std::vector<size_t> chosen = targets(notes);
    if (chosen.empty())
        return 0;
    double first = 1e30, last = -1e30;
    for (size_t i : chosen) {
        first = std::min(first, notes[i].start);
        last = std::max(last, notes[i].start + notes[i].length);
    }
    for (size_t i : chosen) {
        Note &n = notes[i];
        n.start = first + (last - (n.start + n.length));
    }
    return static_cast<int>(chosen.size());
}

int invert(std::vector<Note> &notes)
{
    const std::vector<size_t> chosen = targets(notes);
    if (chosen.empty())
        return 0;
    int low = 127, high = 0;
    for (size_t i : chosen) {
        low = std::min(low, notes[i].key);
        high = std::max(high, notes[i].key);
    }
    for (size_t i : chosen)
        notes[i].key = clampKey(low + high - notes[i].key);
    return static_cast<int>(chosen.size());
}

int chop(std::vector<Note> &notes, double grid)
{
    if (grid <= kMinLength)
        return 0;
    const std::vector<size_t> chosen = targets(notes);
    std::vector<bool> cut(notes.size(), false);
    std::vector<Note> pieces;
    int count = 0;
    for (size_t i : chosen) {
        const Note &n = notes[i];
        if (n.length <= grid + 1e-9)
            continue;
        cut[i] = true;
        ++count;
        for (double t = n.start; t < n.start + n.length - 1e-9; t += grid) {
            Note p = n;
            p.start = t;
            p.length = std::max(kMinLength, std::min(grid, n.start + n.length - t));
            pieces.push_back(p);
        }
    }
    std::vector<Note> out;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (!cut[i])
            out.push_back(notes[i]);
    }
    out.insert(out.end(), pieces.begin(), pieces.end());
    notes = std::move(out);
    return count;
}

int join(std::vector<Note> &notes)
{
    std::vector<size_t> chosen = targets(notes);
    std::sort(chosen.begin(), chosen.end(), [&](size_t a, size_t b) {
        return notes[a].key != notes[b].key ? notes[a].key < notes[b].key : notes[a].start < notes[b].start;
    });
    std::vector<bool> gone(notes.size(), false);
    int joined = 0;
    for (size_t j = 0; j < chosen.size();) {
        Note &head = notes[chosen[j]];
        size_t k = j + 1;
        while (k < chosen.size() && notes[chosen[k]].key == head.key &&
               notes[chosen[k]].start <= head.start + head.length + 1e-6) {
            const Note &n = notes[chosen[k]];
            head.length = std::max(head.length, n.start + n.length - head.start);
            gone[chosen[k]] = true;
            ++joined;
            ++k;
        }
        j = k;
    }
    std::vector<Note> out;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (!gone[i])
            out.push_back(notes[i]);
    }
    notes = std::move(out);
    return joined;
}

int fixOverlaps(std::vector<Note> &notes)
{
    std::vector<size_t> chosen = targets(notes);
    std::sort(chosen.begin(), chosen.end(), [&](size_t a, size_t b) {
        return notes[a].key != notes[b].key ? notes[a].key < notes[b].key : notes[a].start < notes[b].start;
    });
    int changed = 0;
    for (size_t j = 0; j + 1 < chosen.size(); ++j) {
        Note &a = notes[chosen[j]];
        const Note &b = notes[chosen[j + 1]];
        if (a.key == b.key && a.start + a.length > b.start + 1e-9 && b.start > a.start) {
            a.length = std::max(kMinLength, b.start - a.start);
            ++changed;
        }
    }
    return changed;
}

int duplicate(std::vector<Note> &notes, double grid)
{
    std::vector<size_t> chosen;
    for (size_t i = 0; i < notes.size(); ++i) {
        if (notes[i].selected)
            chosen.push_back(i);
    }
    if (chosen.empty())
        return 0;
    double first = 1e30, last = -1e30;
    for (size_t i : chosen) {
        first = std::min(first, notes[i].start);
        last = std::max(last, notes[i].start + notes[i].length);
    }
    // After the selection, on the grid
    double span = last - first;
    if (grid > 0.0)
        span = std::max(grid, std::ceil(span / grid - 1e-9) * grid);
    std::vector<Note> copies;
    for (size_t i : chosen) {
        Note n = notes[i];
        n.start += span;
        copies.push_back(n);
        notes[i].selected = false;
    }
    notes.insert(notes.end(), copies.begin(), copies.end());
    return static_cast<int>(copies.size());
}

int removeSelected(std::vector<Note> &notes)
{
    const size_t before = notes.size();
    notes.erase(std::remove_if(notes.begin(), notes.end(), [](const Note &n) { return n.selected; }), notes.end());
    return static_cast<int>(before - notes.size());
}

void clampToClip(std::vector<Note> &notes, double length, double minimum)
{
    notes.erase(std::remove_if(notes.begin(), notes.end(), [length](const Note &n) { return n.start >= length - 1e-9; }),
                notes.end());
    for (Note &n : notes) {
        n.start = std::max(0.0, n.start);
        n.length = std::clamp(n.length, std::min(minimum, length - n.start), length - n.start);
    }
}

int selectedCount(const std::vector<Note> &notes)
{
    return static_cast<int>(std::count_if(notes.begin(), notes.end(), [](const Note &n) { return n.selected; }));
}

void selectAll(std::vector<Note> &notes, bool selected)
{
    for (Note &n : notes)
        n.selected = selected;
}

std::vector<Note> copySelection(const std::vector<Note> &notes)
{
    std::vector<Note> out;
    for (size_t i : targets(notes))
        out.push_back(notes[i]);
    double first = 1e30;
    for (const Note &n : out)
        first = std::min(first, n.start);
    for (Note &n : out) {
        n.start -= first;
        n.selected = true;
    }
    return out;
}

void paste(std::vector<Note> &notes, const std::vector<Note> &clipboard, double at)
{
    selectAll(notes, false);
    for (Note n : clipboard) {
        n.start += at;
        n.selected = true;
        notes.push_back(n);
    }
}

int toggleMute(std::vector<Note> &notes)
{
    const std::vector<size_t> chosen = targets(notes);
    bool anyPlaying = false;
    for (size_t i : chosen)
        anyPlaying = anyPlaying || !notes[i].muted;
    int changed = 0;
    for (size_t i : chosen) {
        if (notes[i].muted != anyPlaying)
            ++changed;
        notes[i].muted = anyPlaying;
    }
    return changed;
}

int nudge(std::vector<Note> &notes, double beats)
{
    const std::vector<size_t> chosen = targets(notes);
    if (chosen.empty())
        return 0;
    double first = 1e30;
    for (size_t i : chosen)
        first = std::min(first, notes[i].start);
    const double by = std::max(beats, -first);
    if (by == 0.0)
        return 0;
    for (size_t i : chosen)
        notes[i].start += by;
    return static_cast<int>(chosen.size());
}

int resizeBy(std::vector<Note> &notes, double beats, double minimum)
{
    int changed = 0;
    for (size_t i : targets(notes)) {
        const double length = std::max(minimum, notes[i].length + beats);
        if (length != notes[i].length)
            ++changed;
        notes[i].length = length;
    }
    return changed;
}

int stretchAround(std::vector<Note> &notes, double anchor, double factor, double minimum)
{
    if (factor <= 0.0)
        return 0;
    const std::vector<size_t> chosen = targets(notes);
    for (size_t i : chosen) {
        Note &n = notes[i];
        n.start = std::max(0.0, anchor + (n.start - anchor) * factor);
        n.length = std::max(minimum, n.length * factor);
    }
    return static_cast<int>(chosen.size());
}

bool slice(std::vector<Note> &notes, size_t index, double at, double minimum)
{
    if (index >= notes.size())
        return false;
    Note &n = notes[index];
    if (at < n.start + minimum - 1e-9 || at > n.start + n.length - minimum + 1e-9)
        return false;
    Note right = n;
    right.start = at;
    right.length = n.start + n.length - at;
    n.length = at - n.start;
    notes.push_back(right);
    return true;
}

bool insidePolygon(const std::vector<NotePoint> &polygon, NotePoint p)
{
    // Even-odd ray casting
    bool inside = false;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const NotePoint &a = polygon[i], &b = polygon[j];
        if ((a.key > p.key) != (b.key > p.key) && p.beat < (b.beat - a.beat) * (p.key - a.key) / (b.key - a.key) + a.beat)
            inside = !inside;
    }
    return inside;
}

int selectInLasso(std::vector<Note> &notes, const std::vector<NotePoint> &polygon, bool additive)
{
    int count = 0;
    for (Note &n : notes) {
        bool hit = false;
        if (polygon.size() >= 3) {
            const double y = n.key + 0.5;
            for (double t : {0.0, 0.25, 0.5, 0.75, 1.0})
                hit = hit || insidePolygon(polygon, {n.start + n.length * t, y});
        }
        n.selected = hit || (additive && n.selected);
        count += n.selected ? 1 : 0;
    }
    return count;
}

int selectSameKeys(std::vector<Note> &notes)
{
    bool keys[128] = {};
    for (const Note &n : notes) {
        if (n.selected)
            keys[clampKey(n.key)] = true;
    }
    int added = 0;
    for (Note &n : notes) {
        if (!n.selected && keys[clampKey(n.key)]) {
            n.selected = true;
            ++added;
        }
    }
    return added;
}

int selectNext(std::vector<Note> &notes, int direction)
{
    if (notes.empty())
        return -1;
    std::vector<size_t> order(notes.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&notes](size_t a, size_t b) {
        return notes[a].start != notes[b].start ? notes[a].start < notes[b].start : notes[a].key < notes[b].key;
    });
    // From the selected note furthest that way
    int at = -1;
    for (size_t i = 0; i < order.size(); ++i) {
        if (notes[order[i]].selected && (at < 0 || direction > 0))
            at = static_cast<int>(i);
    }
    int next;
    if (at < 0)
        next = direction > 0 ? 0 : static_cast<int>(order.size()) - 1;
    else
        next = std::clamp(at + (direction > 0 ? 1 : -1), 0, static_cast<int>(order.size()) - 1);
    selectAll(notes, false);
    notes[order[static_cast<size_t>(next)]].selected = true;
    return static_cast<int>(order[static_cast<size_t>(next)]);
}

NoteBounds noteBounds(const std::vector<Note> &notes)
{
    NoteBounds b;
    const std::vector<size_t> chosen = targets(notes);
    if (chosen.empty())
        return b;
    b.start = 1e30;
    b.end = -1e30;
    b.low = 127;
    b.high = 0;
    for (size_t i : chosen) {
        b.start = std::min(b.start, notes[i].start);
        b.end = std::max(b.end, notes[i].start + notes[i].length);
        b.low = std::min(b.low, notes[i].key);
        b.high = std::max(b.high, notes[i].key);
    }
    b.count = static_cast<int>(chosen.size());
    return b;
}

double autoGrid(double pixelsPerBeat, double minPixels, bool triplet)
{
    double grid = triplet ? 4.0 * 2.0 / 3.0 : 4.0;
    while (grid / 2.0 >= 1.0 / 64.0 - 1e-12 && grid / 2.0 * pixelsPerBeat >= minPixels)
        grid /= 2.0;
    return grid;
}

std::string gridName(double beats, int beatsPerBar)
{
    if (beats <= 0.0)
        return "Off";
    if (std::fabs(beats - beatsPerBar) < 1e-9)
        return "1 bar";
    // A whole note is four beats; a triplet is two thirds of the plain value
    for (bool triplet : {false, true}) {
        const double plain = triplet ? beats * 1.5 : beats;
        const double division = 4.0 / plain;
        const int d = static_cast<int>(std::round(division));
        if (d >= 1 && std::fabs(division - d) < 1e-6 && (d & (d - 1)) == 0)
            return "1/" + std::to_string(d) + (triplet ? "T" : "");
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.3g beats", beats);
    return text;
}

ControlLane *findLane(std::vector<ControlLane> &lanes, int controller)
{
    for (ControlLane &lane : lanes) {
        if (lane.controller == controller)
            return &lane;
    }
    return nullptr;
}

ControlLane &laneFor(std::vector<ControlLane> &lanes, int controller)
{
    if (ControlLane *lane = findLane(lanes, controller))
        return *lane;
    lanes.push_back({controller, {}});
    return lanes.back();
}

void setControlPoint(ControlLane &lane, double beat, float value, double merge)
{
    beat = std::max(0.0, beat);
    value = std::clamp(value, 0.0f, 1.0f);
    auto &points = lane.points;
    points.erase(std::remove_if(points.begin(), points.end(),
                                [beat, merge](const ControlPoint &p) { return std::fabs(p.beat - beat) <= merge; }),
                 points.end());
    const auto at = std::upper_bound(points.begin(), points.end(), beat,
                                     [](double b, const ControlPoint &p) { return b < p.beat; });
    points.insert(at, {beat, value});
}

int eraseControlPoints(ControlLane &lane, double from, double to)
{
    if (to < from)
        std::swap(from, to);
    auto &points = lane.points;
    const size_t before = points.size();
    points.erase(std::remove_if(points.begin(), points.end(),
                                [from, to](const ControlPoint &p) { return p.beat >= from - 1e-9 && p.beat <= to + 1e-9; }),
                 points.end());
    return static_cast<int>(before - points.size());
}

void controlLine(ControlLane &lane, double from, float fromValue, double to, float toValue)
{
    if (to < from) {
        std::swap(from, to);
        std::swap(fromValue, toValue);
    }
    eraseControlPoints(lane, from, to);
    setControlPoint(lane, from, fromValue, 0.0);
    setControlPoint(lane, to, toValue, 0.0);
}

int thinControlPoints(ControlLane &lane, float tolerance)
{
    auto &points = lane.points;
    if (points.size() < 3)
        return 0;
    const bool stepped = controllerStepped(lane.controller);
    std::vector<ControlPoint> kept{points.front()};
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        const ControlPoint &a = kept.back(), &p = points[i], &b = points[i + 1];
        bool needed;
        if (stepped) {
            needed = p.value != a.value;
        } else {
            const double span = b.beat - a.beat;
            const float expected = span > 0.0 ? a.value + (b.value - a.value) * static_cast<float>((p.beat - a.beat) / span) : a.value;
            needed = std::fabs(expected - p.value) > tolerance;
        }
        if (needed)
            kept.push_back(p);
    }
    kept.push_back(points.back());
    const int dropped = static_cast<int>(points.size() - kept.size());
    points = std::move(kept);
    return dropped;
}

void dropEmptyLanes(std::vector<ControlLane> &lanes)
{
    lanes.erase(std::remove_if(lanes.begin(), lanes.end(), [](const ControlLane &l) { return l.points.empty(); }), lanes.end());
}

}
