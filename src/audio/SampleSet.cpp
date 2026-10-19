#include "SampleSet.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include <miniaudio.h>

#include "Memory.h"

namespace audio {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isAudioFile(const fs::path &path)
{
    const std::string ext = lower(path.extension().string());
    return ext == ".wav" || ext == ".flac" || ext == ".mp3";
}

// Decodes a whole file to interleaved 16-bit at its own rate and channel count (at most 2) into
// zone (pcm, channels, frames, sampleRate, file). With `trim`, a near-silent tail is cut, with a
// short fade so the cut doesn't click.
bool decode(const std::string &path, SampleZone &zone, bool trim, std::string &error)
{
    ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        error = "Can't open " + path;
        return false;
    }
    ma_format format;
    ma_uint32 channels = 0;
    ma_uint32 rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);
    if (channels == 0 || rate == 0) {
        ma_decoder_uninit(&decoder);
        error = "Unknown format in " + path;
        return false;
    }

    // Straight into the kept channels (mono or stereo; more fold down to their first two), with
    // room made once when the length is known, so the buffer isn't regrown and copied
    const uint32_t kept = std::min<uint32_t>(channels, 2);
    auto pcm = std::make_shared<std::vector<int16_t>>();
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder, &length) == MA_SUCCESS && length > 0 && length < (1ull << 31))
        pcm->reserve(static_cast<size_t>(length) * kept);
    std::vector<int16_t> chunk(4096 * channels);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &read);
        for (ma_uint64 f = 0; f < read; ++f) {
            for (uint32_t c = 0; c < kept; ++c)
                pcm->push_back(chunk[static_cast<size_t>(f) * channels + c]);
        }
        if (result != MA_SUCCESS || read < 4096)
            break;
    }
    ma_decoder_uninit(&decoder);

    size_t frames = pcm->size() / kept;
    if (trim && frames > 0) {
        // The last frame louder than about -78 dB, plus a little, faded out
        constexpr int kQuiet = 4;
        size_t last = frames;
        while (last > 0) {
            bool loud = false;
            for (uint32_t c = 0; c < kept; ++c)
                loud = loud || std::abs(static_cast<int>((*pcm)[(last - 1) * kept + c])) > kQuiet;
            if (loud)
                break;
            --last;
        }
        const size_t keep = std::min(frames, last + 256);
        if (keep + 1024 < frames) {
            const size_t fade = std::min<size_t>(256, keep);
            for (size_t i = 0; i < fade; ++i) {
                const float g = static_cast<float>(i) / static_cast<float>(fade);
                for (uint32_t c = 0; c < kept; ++c) {
                    int16_t &s = (*pcm)[(keep - 1 - i) * kept + c];
                    s = static_cast<int16_t>(s * g);
                }
            }
            frames = keep;
            pcm->resize(frames * kept);
        }
    }
    pcm->shrink_to_fit();
    zone.pcm = std::move(pcm);
    zone.channels = kept;
    zone.frames = static_cast<uint32_t>(frames);
    zone.sampleRate = rate;
    zone.file = path;
    if (frames == 0) {
        error = "No audio in " + path;
        return false;
    }
    return true;
}

// A sample for a zone: streamed when the manager streams and it's long and unlooped (`trim`
// set), from the cache when the cache has it; else decoded whole
bool loadSample(const std::string &path, SampleZone &zone, bool trim, MemoryManager *memory, std::string &error)
{
    const bool stream = memory && trim && memory->streaming();
    if (stream) {
        if (std::shared_ptr<StreamSource> cached = memory->openCached(path)) {
            zone.pcm = cached->preload;
            zone.channels = cached->channels;
            zone.frames = cached->frames;
            zone.sampleRate = cached->sampleRate;
            zone.file = path;
            zone.stream = std::move(cached);
            return true;
        }
    }
    if (!decode(path, zone, trim, error))
        return false;
    if (stream && memory->worthStreaming(zone.frames)) {
        std::string why;
        if (std::shared_ptr<StreamSource> made = memory->create(path, *zone.pcm, zone.channels, zone.sampleRate, why)) {
            zone.pcm = made->preload; // the whole decode goes; only the preload stays
            zone.stream = std::move(made);
        }
        // A cache that can't be written leaves the sample whole in memory
    }
    return true;
}

// An SFZ key: a number or a note name
int parseKey(const std::string &value, int fallback)
{
    if (value.empty())
        return fallback;
    if (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '-')
        return std::clamp(std::atoi(value.c_str()), 0, 127);
    const int key = keyFromName(value);
    return key >= 0 ? key : fallback;
}

using Opcodes = std::map<std::string, std::string>;

// Splits SFZ text into headers and opcodes. Values run to the next space, except for paths
// (sample, default_path), which may hold spaces and run to the next opcode or header.
void parseSfz(const std::string &text, const std::function<void(const std::string &)> &header,
              const std::function<void(const std::string &, const std::string &)> &opcode)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t comment = line.find("//");
        if (comment != std::string::npos)
            line.erase(comment);
        size_t i = 0;
        while (i < line.size()) {
            if (std::isspace(static_cast<unsigned char>(line[i]))) {
                ++i;
                continue;
            }
            if (line[i] == '<') {
                const size_t end = line.find('>', i);
                if (end == std::string::npos)
                    break;
                header(lower(line.substr(i + 1, end - i - 1)));
                i = end + 1;
                continue;
            }
            const size_t equals = line.find('=', i);
            if (equals == std::string::npos)
                break;
            const std::string key = lower(line.substr(i, equals - i));
            size_t end = equals + 1;
            if (key == "sample" || key == "default_path") {
                // Up to the start of the next "name=" or header
                size_t next = line.size();
                for (size_t j = equals + 1; j < line.size(); ++j) {
                    if (line[j] == '<') {
                        next = j;
                        break;
                    }
                    if (std::isspace(static_cast<unsigned char>(line[j]))) {
                        size_t k = j;
                        while (k < line.size() && std::isspace(static_cast<unsigned char>(line[k])))
                            ++k;
                        size_t w = k;
                        while (w < line.size() && (std::isalnum(static_cast<unsigned char>(line[w])) || line[w] == '_'))
                            ++w;
                        if (w > k && w < line.size() && line[w] == '=') {
                            next = j;
                            break;
                        }
                    }
                }
                end = next;
            } else {
                while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '<')
                    ++end;
            }
            std::string value = line.substr(equals + 1, end - equals - 1);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                value.pop_back();
            opcode(key, value);
            i = end;
        }
    }
}

}

const SampleZone *SampleSet::zoneFor(int key, int velocity) const
{
    const SampleZone *nearest = nullptr;
    const SampleZone *nearestLayer = nullptr; // nearest by key among zones for this velocity
    int distance = 1 << 30;
    int layerDistance = 1 << 30;
    for (const SampleZone &zone : zones) {
        const bool layer = velocity >= zone.lowVelocity && velocity <= zone.highVelocity;
        if (key >= zone.lowKey && key <= zone.highKey && layer)
            return &zone;
        const int d = key < zone.lowKey ? zone.lowKey - key : key > zone.highKey ? key - zone.highKey : 0;
        if (layer && d < layerDistance) {
            layerDistance = d;
            nearestLayer = &zone;
        }
        if (d < distance) {
            distance = d;
            nearest = &zone;
        }
    }
    return nearestLayer ? nearestLayer : nearest;
}

int SampleSet::zonesFor(int key, int velocity, const SampleZone **out, int max) const
{
    int count = 0;
    for (const SampleZone &zone : zones) {
        if (count < max && key >= zone.lowKey && key <= zone.highKey && velocity >= zone.lowVelocity &&
            velocity <= zone.highVelocity)
            out[count++] = &zone;
    }
    if (count == 0 && max > 0) {
        if (const SampleZone *z = zoneFor(key, velocity))
            out[count++] = z;
    }
    return count;
}

uint64_t SampleSet::bytes() const
{
    std::set<const std::vector<int16_t> *> seen;
    uint64_t total = 0;
    for (const SampleZone &zone : zones) {
        if (zone.pcm && seen.insert(zone.pcm.get()).second)
            total += zone.pcm->size() * sizeof(int16_t);
    }
    return total;
}

uint64_t SampleSet::streamedBytes() const
{
    std::set<const StreamSource *> seen;
    uint64_t total = 0;
    for (const SampleZone &zone : zones) {
        if (zone.stream && seen.insert(zone.stream.get()).second)
            total += static_cast<uint64_t>(zone.stream->frames - zone.stream->preloadFrames) * zone.stream->channels *
                     sizeof(int16_t);
    }
    return total;
}

void SampleZone::setSamples(const std::vector<float> &interleaved)
{
    auto out = std::make_shared<std::vector<int16_t>>(interleaved.size());
    for (size_t i = 0; i < interleaved.size(); ++i)
        (*out)[i] = static_cast<int16_t>(std::lround(std::clamp(interleaved[i], -1.0f, 32767.0f / 32768.0f) * 32768.0f));
    frames = channels ? static_cast<uint32_t>(interleaved.size() / channels) : 0;
    pcm = std::move(out);
}

int keyFromName(const std::string &name)
{
    if (name.empty())
        return -1;
    static const int semitones[] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    if (letter < 'A' || letter > 'G')
        return -1;
    int semitone = semitones[letter - 'A'];
    size_t i = 1;
    if (i < name.size() && name[i] == '#') {
        ++semitone;
        ++i;
    } else if (i < name.size() && (name[i] == 'b' || name[i] == 'B') && i + 1 < name.size()) {
        --semitone;
        ++i;
    }
    if (i >= name.size())
        return -1;
    char *end = nullptr;
    const long octave = std::strtol(name.c_str() + i, &end, 10);
    if (end == name.c_str() + i || *end != '\0')
        return -1;
    const long key = (octave + 1) * 12 + semitone;
    return key >= 0 && key <= 127 ? static_cast<int>(key) : -1;
}

std::string keyName(int key)
{
    static const char *names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    key = std::clamp(key, 0, 127);
    return std::string(names[key % 12]) + std::to_string(key / 12 - 1);
}

namespace {

std::string trim(const std::string &s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Replaces each $NAME with its #define, longest name first
std::string substitute(const std::string &line, const std::map<std::string, std::string> &defines)
{
    if (defines.empty() || line.find('$') == std::string::npos)
        return line;
    std::string out;
    for (size_t i = 0; i < line.size();) {
        if (line[i] != '$') {
            out += line[i++];
            continue;
        }
        size_t best = 0;
        const std::string *value = nullptr;
        for (const auto &[name, v] : defines) {
            if (name.size() > best && line.compare(i, name.size(), name) == 0) {
                best = name.size();
                value = &v;
            }
        }
        if (value) {
            out += *value;
            i += best;
        } else {
            out += line[i++];
        }
    }
    return out;
}

// An SFZ's text with its #includes expanded and its #defines substituted. Included paths are
// relative to `base`, the first file's folder.
bool expandSfz(const fs::path &file, const fs::path &base, std::map<std::string, std::string> &defines,
               std::string &out, std::vector<std::string> *includes, int depth, std::string &error)
{
    if (depth > 16) {
        error = "#include nests too deeply at " + file.string();
        return false;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "Can't open " + file.string();
        return false;
    }
    std::stringstream whole;
    whole << in.rdbuf();
    std::istringstream lines(whole.str());
    std::string line;
    bool inBlock = false; // inside a /* */ comment
    while (std::getline(lines, line)) {
        // Block comments, which may span lines; line comments are kept ("//+ Name:" is read)
        std::string kept;
        for (size_t i = 0; i < line.size(); ++i) {
            if (inBlock) {
                if (line.compare(i, 2, "*/") == 0) {
                    inBlock = false;
                    ++i;
                }
                continue;
            }
            if (line.compare(i, 2, "/*") == 0) {
                inBlock = true;
                ++i;
                continue;
            }
            if (line.compare(i, 2, "//") == 0) {
                kept.append(line, i, std::string::npos);
                break;
            }
            kept += line[i];
        }
        const std::string t = trim(kept);
        if (t.rfind("#define", 0) == 0) {
            std::istringstream words(t.substr(7));
            std::string name, value;
            words >> name;
            std::getline(words, value);
            const size_t comment = value.find("//");
            if (comment != std::string::npos)
                value.erase(comment);
            value = trim(value);
            if (!name.empty() && name[0] == '$')
                defines[name] = substitute(value, defines);
            continue;
        }
        if (t.rfind("#include", 0) == 0) {
            const size_t open = t.find('"');
            const size_t close = open == std::string::npos ? open : t.find('"', open + 1);
            if (close == std::string::npos)
                continue;
            std::string relative = substitute(t.substr(open + 1, close - open - 1), defines);
            std::replace(relative.begin(), relative.end(), '\\', '/');
            if (includes && std::find(includes->begin(), includes->end(), relative) == includes->end())
                includes->push_back(relative);
            if (!expandSfz(base / fs::u8path(relative), base, defines, out, includes, depth + 1, error))
                return false;
            continue;
        }
        out += substitute(kept, defines);
        out += '\n';
    }
    return true;
}

int number(const Opcodes &ops, const char *key, int fallback)
{
    auto it = ops.find(key);
    return it == ops.end() || it->second.empty() ? fallback : std::atoi(it->second.c_str());
}

float real(const Opcodes &ops, const char *key, float fallback)
{
    auto it = ops.find(key);
    return it == ops.end() || it->second.empty() ? fallback : static_cast<float>(std::atof(it->second.c_str()));
}

std::string text(const Opcodes &ops, const char *key)
{
    auto it = ops.find(key);
    return it == ops.end() ? std::string() : lower(it->second);
}

}

std::shared_ptr<SampleSet> loadSfz(const std::string &path, std::string &error, MemoryManager *memory)
{
    const fs::path file = fs::u8path(path);
    const fs::path base = file.parent_path();
    std::map<std::string, std::string> defines;
    std::string content;
    if (!expandSfz(file, base, defines, content, nullptr, 0, error))
        return nullptr;

    auto set = std::make_shared<SampleSet>();
    set->name = file.stem().string();

    // Every region with the opcodes it inherits, before choosing which to keep
    Opcodes global, master, group, region;
    std::string level; // the header the opcodes belong to
    bool inRegion = false;
    std::string defaultPath;
    std::vector<Opcodes> regions;
    auto commit = [&]() {
        if (!inRegion)
            return;
        inRegion = false;
        Opcodes merged = global;
        for (const Opcodes *layer : {&master, &group, &region}) {
            for (const auto &[k, v] : *layer)
                merged[k] = v;
        }
        if (merged.count("sample")) {
            merged["sample"] = defaultPath + merged["sample"];
            regions.push_back(std::move(merged));
        }
    };
    parseSfz(
        content,
        [&](const std::string &header) {
            commit();
            level = header;
            if (header == "global") {
                global.clear();
                master.clear();
                group.clear();
            } else if (header == "master") {
                master.clear();
                group.clear();
            } else if (header == "group") {
                group.clear();
            } else if (header == "region") {
                region.clear();
                inRegion = true;
            }
        },
        [&](const std::string &key, const std::string &value) {
            if (key == "default_path") {
                defaultPath = value;
                std::replace(defaultPath.begin(), defaultPath.end(), '\\', '/');
                if (!defaultPath.empty() && defaultPath.back() != '/')
                    defaultPath += '/';
            } else if (level == "region") {
                region[key] = value;
            } else if (level == "group") {
                group[key] = value;
            } else if (level == "master") {
                master[key] = value;
            } else if (level != "control" && level != "curve" && level != "effect") {
                global[key] = value; // <global>, or opcodes before any header
            }
        });
    commit();

    // Attack regions only, the first round robin, one keyswitch
    std::string keyswitch;
    for (const char *opcode : {"sw_default", "sw_last"}) {
        for (const Opcodes &r : regions) {
            auto it = r.find(opcode);
            if (it != r.end()) {
                keyswitch = it->second;
                break;
            }
        }
        if (!keyswitch.empty())
            break;
    }
    std::vector<const Opcodes *> kept;
    for (const Opcodes &r : regions) {
        const std::string trigger = text(r, "trigger");
        if (!trigger.empty() && trigger != "attack" && trigger != "first" && trigger != "legato")
            continue;
        if (number(r, "seq_position", 1) > 1 || real(r, "lorand", 0.0f) > 0.0f)
            continue;
        auto sw = r.find("sw_last");
        if (sw != r.end() && !keyswitch.empty() && parseKey(sw->second, -1) != parseKey(keyswitch, -2))
            continue;
        kept.push_back(&r);
    }

    // Zones, without their audio yet
    struct Pending {
        SampleZone zone;
        std::string file;
    };
    std::vector<Pending> pending;
    bool releaseSet = false;
    for (const Opcodes *rp : kept) {
        const Opcodes &r = *rp;
        Pending p;
        std::string relative = r.at("sample");
        std::replace(relative.begin(), relative.end(), '\\', '/');
        p.file = (base / fs::u8path(relative)).string();
        SampleZone &zone = p.zone;
        const int key = parseKey(text(r, "key"), -1);
        zone.lowKey = parseKey(text(r, "lokey"), key >= 0 ? key : 0);
        zone.highKey = parseKey(text(r, "hikey"), key >= 0 ? key : 127);
        zone.rootKey = parseKey(text(r, "pitch_keycenter"), key >= 0 ? key : 60);
        zone.lowVelocity = std::clamp(number(r, "lovel", 1), 1, 127);
        zone.highVelocity = std::clamp(number(r, "hivel", 127), zone.lowVelocity, 127);
        zone.gain = std::pow(10.0f, real(r, "volume", 0.0f) / 20.0f) * real(r, "amplitude", 100.0f) / 100.0f;
        zone.tuneCents = real(r, "tune", 0.0f) + 100.0f * real(r, "transpose", 0.0f);
        zone.keytrackCents = real(r, "pitch_keytrack", 100.0f);
        const std::string loop = text(r, "loop_mode");
        if ((loop == "loop_continuous" || loop == "loop_sustain") && r.count("loop_end")) {
            zone.loopStart = static_cast<uint32_t>(std::max(0, number(r, "loop_start", 0)));
            zone.loopEnd = static_cast<uint32_t>(std::max(0, number(r, "loop_end", 0)));
        }
        if (r.count("ampeg_release") && !releaseSet) {
            set->releaseSeconds = std::clamp(real(r, "ampeg_release", 0.3f), 0.005f, 10.0f);
            releaseSet = true;
        }
        if (zone.lowKey <= zone.highKey)
            pending.push_back(std::move(p));
    }

    // Many velocity layers take a lot of memory: keep kMaxVelocityLayers per key range, spread
    // over the range, each widened down to the one kept below it
    using Range = std::pair<int, int>;
    std::map<Range, std::vector<Range>> layers; // key range: its velocity ranges
    for (const Pending &p : pending) {
        auto &v = layers[{p.zone.lowKey, p.zone.highKey}];
        const Range range{p.zone.lowVelocity, p.zone.highVelocity};
        if (std::find(v.begin(), v.end(), range) == v.end())
            v.push_back(range);
    }
    std::map<Range, std::map<Range, Range>> remap; // key range: old velocity range to new
    for (auto &[keys, ranges] : layers) {
        std::sort(ranges.begin(), ranges.end());
        auto &m = remap[keys];
        const size_t n = ranges.size();
        const size_t k = static_cast<size_t>(kMaxVelocityLayers);
        if (n <= k) {
            for (const Range &r : ranges)
                m[r] = r;
            continue;
        }
        int low = 1;
        for (size_t i = 0; i < k; ++i) {
            const Range &r = ranges[(i * (n - 1) + (k - 1) / 2) / (k - 1)];
            const int high = i + 1 == k ? 127 : std::max(low, r.second);
            m[r] = {low, high};
            low = high + 1;
        }
    }

    // The audio, each file decoded once and shared; tails trimmed where nothing loops
    std::set<std::string> looped;
    for (const Pending &p : pending) {
        if (p.zone.loopEnd > p.zone.loopStart)
            looped.insert(p.file);
    }
    std::map<std::string, SampleZone> decoded;
    uint64_t bytes = 0;
    for (Pending &p : pending) {
        const auto &m = remap[{p.zone.lowKey, p.zone.highKey}];
        auto layer = m.find({p.zone.lowVelocity, p.zone.highVelocity});
        if (layer == m.end())
            continue;
        auto found = decoded.find(p.file);
        if (found == decoded.end()) {
            SampleZone audio;
            if (!loadSample(p.file, audio, looped.count(p.file) == 0, memory, error))
                return nullptr;
            bytes += audio.pcm->size() * sizeof(int16_t);
            if (bytes > kMaxSampleBytes) {
                error = "Too big to load: over " + std::to_string(kMaxSampleBytes >> 20) + " MB of samples";
                return nullptr;
            }
            found = decoded.emplace(p.file, std::move(audio)).first;
        }
        SampleZone zone = p.zone;
        zone.lowVelocity = layer->second.first;
        zone.highVelocity = layer->second.second;
        const SampleZone &audio = found->second;
        zone.pcm = audio.pcm; // shared, not copied
        zone.stream = audio.stream;
        zone.channels = audio.channels;
        zone.frames = audio.frames;
        zone.sampleRate = audio.sampleRate;
        zone.file = audio.file;
        if (zone.frames < 3 || zone.loopEnd >= zone.frames - 1)
            zone.loopEnd = zone.frames >= 3 ? zone.frames - 2 : 0;
        if (zone.loopEnd <= zone.loopStart + 1)
            zone.loopStart = zone.loopEnd = 0;
        set->zones.push_back(std::move(zone));
    }

    if (set->zones.empty()) {
        error = "No samples in " + path;
        return nullptr;
    }
    return set;
}

std::vector<std::string> sfzSamplePaths(const std::string &path, std::string &displayName, std::string &error,
                                        std::vector<std::string> *includes)
{
    const fs::path file = fs::u8path(path);
    std::map<std::string, std::string> defines;
    std::string content;
    if (!expandSfz(file, file.parent_path(), defines, content, includes, 0, error))
        return {};

    // FreePats and others name the instrument in a comment: "//+ Name: Spanish classical guitar"
    const size_t tag = content.find("//+ Name:");
    if (tag != std::string::npos) {
        const size_t end = content.find_first_of("\r\n", tag);
        displayName = trim(content.substr(tag + 9, end == std::string::npos ? std::string::npos : end - tag - 9));
    }

    std::vector<std::string> paths;
    std::string defaultPath;
    parseSfz(
        content, [](const std::string &) {},
        [&](const std::string &key, const std::string &value) {
            if (key == "default_path") {
                defaultPath = value;
                std::replace(defaultPath.begin(), defaultPath.end(), '\\', '/');
                if (!defaultPath.empty() && defaultPath.back() != '/')
                    defaultPath += '/';
            } else if (key == "sample") {
                std::string relative = defaultPath + value;
                std::replace(relative.begin(), relative.end(), '\\', '/');
                if (std::find(paths.begin(), paths.end(), relative) == paths.end())
                    paths.push_back(relative);
            }
        });
    return paths;
}

std::string findSfz(const std::string &folder)
{
    // The shallowest .sfz, then by name: instruments from repositories often keep theirs in a
    // subfolder ("Programs", "jRhodes3c-flac-sfz")
    std::vector<std::pair<int, fs::path>> found;
    std::error_code ec;
    const fs::path root = fs::u8path(folder);
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it.depth() >= 2)
            it.disable_recursion_pending();
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && lower(it->path().extension().string()) == ".sfz")
            found.emplace_back(it.depth(), it->path());
    }
    if (found.empty())
        return {};
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first < b.first : lower(a.second.string()) < lower(b.second.string());
    });
    return found.front().second.string();
}

std::shared_ptr<SampleSet> loadNoteFolder(const std::string &folder, std::string &error, MemoryManager *memory)
{
    std::vector<std::pair<int, fs::path>> files;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(fs::u8path(folder), ec)) {
        if (!entry.is_regular_file() || !isAudioFile(entry.path()))
            continue;
        const int key = keyFromName(entry.path().stem().string());
        if (key >= 0)
            files.emplace_back(key, entry.path());
    }
    if (files.empty()) {
        error = "No note-named audio files (like E2.wav) in " + folder;
        return nullptr;
    }
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    auto set = std::make_shared<SampleSet>();
    set->name = fs::u8path(folder).filename().string();
    for (size_t i = 0; i < files.size(); ++i) {
        SampleZone zone;
        if (!loadSample(files[i].second.string(), zone, true, memory, error))
            return nullptr;
        const int key = files[i].first;
        zone.rootKey = key;
        zone.lowKey = i == 0 ? 0 : (files[i - 1].first + key) / 2 + 1;
        zone.highKey = i + 1 == files.size() ? 127 : (key + files[i + 1].first) / 2;
        set->zones.push_back(std::move(zone));
    }
    return set;
}

std::shared_ptr<SampleSet> loadInstrumentFolder(const std::string &folder, std::string &error, MemoryManager *memory)
{
    std::error_code ec;
    if (!fs::is_directory(fs::u8path(folder), ec)) {
        error = "Can't read " + folder;
        return nullptr;
    }
    const std::string sfz = findSfz(folder);
    std::shared_ptr<SampleSet> set = sfz.empty() ? loadNoteFolder(folder, error, memory) : loadSfz(sfz, error, memory);
    if (set)
        set->name = fs::u8path(folder).filename().string();
    return set;
}

}
