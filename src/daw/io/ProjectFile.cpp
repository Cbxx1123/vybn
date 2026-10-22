#include "daw/io/ProjectFile.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "daw/model/Routing.h"
#include "daw/io/Toml.h"
#include "audio/Effects.h"
#include "audio/Encoders.h"
#include "audio/PluginInstrument.h"
#include "audio/Synth.h"

namespace daw {

namespace fs = std::filesystem;

std::vector<std::string> instrumentsUsed(const Project &project)
{
    std::vector<std::string> names;
    for (const Track &t : project.tracks) {
        if (t.kind == TrackKind::Instrument && t.instrument.kind == audio::DeviceKind::Sampler &&
            std::find(names.begin(), names.end(), t.instrument.id) == names.end())
            names.push_back(t.instrument.id);
    }
    return names;
}

namespace {

// A path as it's written: relative when it's inside the project's folder
std::string relativeTo(const std::string &path, const std::string &folder)
{
    if (folder.empty() || path.empty())
        return path;
    std::error_code ec;
    const fs::path relative = fs::relative(fs::path(path), fs::path(folder), ec);
    if (ec || relative.empty())
        return path;
    const std::string text = relative.generic_string();
    return text.rfind("..", 0) == 0 ? fs::path(path).generic_string() : text;
}

std::string resolve(const std::string &path, const std::string &folder)
{
    if (folder.empty() || path.empty() || fs::path(path).is_absolute())
        return path;
    return (fs::path(folder) / path).lexically_normal().string();
}

}

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string toBase64(const std::string &data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const uint32_t a = static_cast<unsigned char>(data[i]);
        const uint32_t b = i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) : 0;
        const uint32_t c = i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += kBase64[(v >> 18) & 63];
        out += kBase64[(v >> 12) & 63];
        out += i + 1 < data.size() ? kBase64[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? kBase64[v & 63] : '=';
    }
    return out;
}

bool fromBase64(const std::string &text, std::string &out)
{
    out.clear();
    uint32_t v = 0;
    int bits = 0;
    for (char ch : text) {
        if (ch == '=')
            break;
        const char *at = std::strchr(kBase64, ch);
        if (!at || !ch)
            return false;
        v = (v << 6) | static_cast<uint32_t>(at - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return true;
}

// Automation lanes as `automation = [...]` in the table being written: each its parameter (a
// track's own as "volume" or "pan", a device's by id) and its points as {beat, value}. Nothing for none.
// A track's kind as the file names it
const char *kindKey(TrackKind kind)
{
    switch (kind) {
    case TrackKind::Instrument:
        return "instrument";
    case TrackKind::Return:
        return "return";
    case TrackKind::Group:
        return "group";
    case TrackKind::Audio:
        break;
    }
    return "audio";
}

void writeAutomation(std::ostringstream &out, const std::vector<audio::AutomationLane> &lanes, bool track,
                     const char *key = "automation")
{
    if (lanes.empty())
        return;
    out << key << " = [\n";
    for (const audio::AutomationLane &lane : lanes) {
        out << "  { param = ";
        if (lane.param == audio::kVolumeParam)
            out << "\"volume\"";
        else if (lane.param == audio::kPanParam)
            out << "\"pan\"";
        else if (track && isSendParam(lane.param))
            out << "\"send\", to = " << sendTarget(lane.param);
        else
            out << lane.param;
        out << ", points = [";
        for (size_t i = 0; i < lane.points.size(); ++i)
            out << (i ? ", " : " ") << "{ beat = " << toml::number(lane.points[i].beat) << ", value = "
                << toml::number(lane.points[i].value) << " }";
        out << (lane.points.empty() ? "] },\n" : " ] },\n");
    }
    out << "]\n";
}

// One device as a table under `header` ("[tracks.instrument]" or "[[tracks.effects]]"): its kind,
// id, name, whether it is bypassed, the track an effect is keyed by (sidechain, when set), a plugin's state (base64), its values as {id, value} tables and
// its automation
void writeDevice(std::ostringstream &out, const char *header, const audio::DeviceSetting &d)
{
    out << "\n" << header << "\n";
    out << "kind = " << toml::quote(audio::deviceKindKey(d.kind)) << "\n";
    out << "id = " << toml::quote(d.id) << "\n";
    out << "name = " << toml::quote(d.name) << "\n";
    out << "bypass = " << (d.bypass ? "true" : "false") << "\n";
    if (d.sidechain != 0)
        out << "sidechain = " << d.sidechain << "\n";
    if (d.state && !d.state->empty())
        out << "state = " << toml::quote(toBase64(*d.state)) << "\n";
    writeAutomation(out, d.automation, false);
    if (d.values.empty()) {
        out << "params = []\n";
        return;
    }
    out << "params = [\n";
    for (const auto &v : d.values)
        out << "  { id = " << v.first << ", value = " << toml::number(v.second) << " },\n";
    out << "]\n";
}

}

namespace {

// One export target as a table under "[[export_presets.targets]]": every field, enums by name
void writeExportTarget(std::ostringstream &out, const ExportTarget &t)
{
    using toml::number;
    using toml::quote;
    auto boolean = [](bool b) { return b ? "true" : "false"; };
    out << "\n[[export_presets.targets]]\n";
    out << "enabled = " << boolean(t.enabled) << "\n";
    out << "name = " << quote(t.name) << "\n";
    out << "source = " << quote(enumName(t.source)) << "\n";
    out << "tracks = [";
    for (size_t i = 0; i < t.tracks.size(); ++i)
        out << (i ? ", " : "") << quote(t.tracks[i]);
    out << "]\n";
    out << "tap = " << quote(enumName(t.tap)) << "\n";
    out << "respect_mute_solo = " << boolean(t.respectMuteSolo) << "\n";
    out << "range = " << quote(enumName(t.range)) << "\n";
    out << "from_bar = " << number(t.fromBar) << "\n";
    out << "to_bar = " << number(t.toBar) << "\n";
    out << "tail_seconds = " << number(t.tailSeconds) << "\n";
    out << "format = " << quote(enumName(t.format)) << "\n";
    out << "bits = " << t.bits << "\n";
    out << "bitrate = " << t.bitrate << "\n";
    out << "sample_rate = " << t.sampleRate << "\n";
    out << "mono = " << boolean(t.mono) << "\n";
    out << "normalize = " << quote(enumName(t.normalize)) << "\n";
    out << "peak_db = " << number(t.peakDb) << "\n";
    out << "loudness_lufs = " << number(t.loudnessLufs) << "\n";
    out << "limiter = " << boolean(t.limiter) << "\n";
    out << "ceiling_dbtp = " << number(t.ceilingDbtp) << "\n";
    out << "fade_in_ms = " << number(t.fadeInMs) << "\n";
    out << "fade_out_ms = " << number(t.fadeOutMs) << "\n";
    out << "dither = " << quote(enumName(t.dither)) << "\n";
    out << "file_name = " << quote(t.fileName) << "\n";
    out << "folder = " << quote(t.folder) << "\n";
}

}

std::string writeProject(const Project &project, const std::string &folder)
{
    using toml::number;
    using toml::quote;
    std::ostringstream out;
    auto boolean = [](bool b) { return b ? "true" : "false"; };

    out << "# VYBN project\n";
    out << "format = " << kProjectFormat << "\n";
    out << "name = " << quote(project.name) << "\n";
    out << "tempo = " << number(project.tempo) << "\n";
    out << "beats_per_bar = " << project.beatsPerBar << "\n";
    out << "bars = " << project.bars << "\n";
    if (project.masterVolume != 1.0f)
        out << "master_volume = " << number(project.masterVolume) << "\n";
    writeAutomation(out, project.masterAutomation, true, "master_automation");
    if (project.hasLoop()) {
        out << "loop_start = " << number(project.loopStart) << "\n";
        out << "loop_end = " << number(project.loopEnd) << "\n";
        out << "looping = " << boolean(project.looping) << "\n";
    }
    out << "# The sample library instruments the tracks play\n";
    out << "instruments = [";
    const std::vector<std::string> used = instrumentsUsed(project);
    for (size_t i = 0; i < used.size(); ++i)
        out << (i ? ", " : "") << quote(used[i]);
    out << "]\n";
    // No presets is written as an empty array, so it reads back as none rather than the defaults
    if (project.exportPresets.empty())
        out << "export_presets = []\n";

    for (const Track &t : project.tracks) {
        const bool instrument = t.kind == TrackKind::Instrument;
        const bool audio = t.kind == TrackKind::Audio;
        out << "\n[[tracks]]\n";
        if (t.id != kNoId)
            out << "id = " << t.id << "\n";
        out << "name = " << quote(t.name) << "\n";
        out << "kind = " << quote(kindKey(t.kind)) << "\n";
        out << "gain = " << number(t.gain) << "\n";
        out << "pan = " << number(t.pan) << "\n";
        out << "mute = " << boolean(t.mute) << "\n";
        out << "solo = " << boolean(t.solo) << "\n";
        out << "armed = " << boolean(t.armed) << "\n";
        if (t.kind == TrackKind::Group && t.folded)
            out << "folded = true\n";
        if (audio) {
            out << "input_channel = " << t.inputChannel << "\n";
            out << "input_stereo = " << boolean(t.inputStereo) << "\n";
            out << "monitor = " << boolean(t.monitor) << "\n";
        }
        // Where its sound goes: a group (its id; none for the master), and its sends
        if (t.output != kNoId)
            out << "output = " << t.output << "\n";
        if (!t.sends.empty()) {
            out << "sends = [";
            for (size_t i = 0; i < t.sends.size(); ++i)
                out << (i ? ", " : " ") << "{ to = " << t.sends[i].to << ", level = " << number(t.sends[i].level)
                    << ", pre_fader = " << boolean(t.sends[i].preFader) << " }";
            out << " ]\n";
        }
        writeAutomation(out, t.automation, true);
        // Its devices: the instrument, then the effects
        if (instrument && t.hasInstrument())
            writeDevice(out, "[tracks.instrument]", t.instrument);
        for (const audio::DeviceSetting &d : t.effects)
            writeDevice(out, "[[tracks.effects]]", d);
        for (const Clip &c : t.clips) {
            out << "\n[[tracks.clips]]\n";
            if (c.id != kNoId)
                out << "id = " << c.id << "\n";
            out << "name = " << quote(c.name) << "\n";
            out << "start = " << number(c.start) << "\n";
            out << "length = " << number(c.length) << "\n";
            if (!instrument) {
                if (c.hasAudio()) {
                    out << "audio_file = " << quote(relativeTo(c.audioFile, folder)) << "\n";
                    out << "offset = " << number(c.offset) << "\n";
                    out << "gain = " << number(c.gain) << "\n";
                    out << "fade_in = " << number(c.fadeIn) << "\n";
                    out << "fade_out = " << number(c.fadeOut) << "\n";
                }
                continue;
            }
            // Controller lanes: each its points as {beat, value}
            if (!c.controls.empty()) {
                out << "controls = [\n";
                for (const ControlLane &lane : c.controls) {
                    out << "  { controller = " << lane.controller << ", points = [";
                    for (size_t i = 0; i < lane.points.size(); ++i)
                        out << (i ? ", " : " ") << "{ beat = " << number(lane.points[i].beat) << ", value = "
                            << number(lane.points[i].value) << " }";
                    out << " ] },\n";
                }
                out << "]\n";
            }
            if (c.notes.empty()) {
                out << "notes = []\n";
                continue;
            }
            out << "notes = [\n";
            for (const Note &n : c.notes) {
                out << "  { start = " << number(n.start) << ", length = " << number(n.length) << ", key = " << n.key
                    << ", velocity = " << number(n.velocity) << (n.muted ? ", muted = true" : "") << " },\n";
            }
            out << "]\n";
        }
    }
    for (const audio::DeviceSetting &d : project.masterEffects)
        writeDevice(out, "[[master_effects]]", d);
    // The export details, only the fields that have something in them
    const ExportMetadata &meta = project.exportMetadata;
    if (!meta.title.empty() || !meta.artist.empty() || !meta.album.empty() || !meta.comment.empty()) {
        out << "\n[export_metadata]\n";
        if (!meta.title.empty())
            out << "title = " << quote(meta.title) << "\n";
        if (!meta.artist.empty())
            out << "artist = " << quote(meta.artist) << "\n";
        if (!meta.album.empty())
            out << "album = " << quote(meta.album) << "\n";
        if (!meta.comment.empty())
            out << "comment = " << quote(meta.comment) << "\n";
    }
    for (const ExportPreset &preset : project.exportPresets) {
        out << "\n[[export_presets]]\n";
        out << "name = " << quote(preset.name) << "\n";
        for (const ExportTarget &target : preset.targets)
            writeExportTarget(out, target);
    }
    return out.str();
}

namespace {

// Reads typed fields from a table, failing on the first value of the wrong type. Missing keys keep
// whatever the target already holds.
class FieldReader {
public:
    explicit FieldReader(std::string &error) : error_(error) {}

    bool ok() const { return error_.empty(); }

    bool fail(const std::string &what)
    {
        if (error_.empty())
            error_ = what;
        return false;
    }

    bool text(const toml::Value &table, const char *key, const std::string &where, std::string &out)
    {
        const toml::Value *v = table.at(key);
        if (!v)
            return true;
        if (v->type() != toml::Type::String)
            return wrong(where, key, "a string", *v);
        out = v->string();
        return true;
    }

    bool real(const toml::Value &table, const char *key, const std::string &where, double &out)
    {
        const toml::Value *v = table.at(key);
        if (!v)
            return true;
        if (!v->isNumber() || !std::isfinite(v->number()))
            return wrong(where, key, "a number", *v);
        out = v->number();
        return true;
    }

    bool real(const toml::Value &table, const char *key, const std::string &where, float &out)
    {
        double d = out;
        if (!real(table, key, where, d))
            return false;
        out = static_cast<float>(d);
        return true;
    }

    bool whole(const toml::Value &table, const char *key, const std::string &where, int &out)
    {
        const toml::Value *v = table.at(key);
        if (!v)
            return true;
        if (v->type() != toml::Type::Integer)
            return wrong(where, key, "an integer", *v);
        out = static_cast<int>(std::clamp<int64_t>(v->integer(), -1000000, 1000000));
        return true;
    }

    bool flag(const toml::Value &table, const char *key, const std::string &where, bool &out)
    {
        const toml::Value *v = table.at(key);
        if (!v)
            return true;
        if (v->type() != toml::Type::Boolean)
            return wrong(where, key, "true or false", *v);
        out = v->boolean();
        return true;
    }

    // An array of tables; nullptr (and no error) when the key is missing
    const toml::Array *tables(const toml::Value &table, const char *key, const std::string &where)
    {
        const toml::Value *v = table.at(key);
        if (!v)
            return nullptr;
        if (!v->isArray()) {
            wrong(where, key, "an array", *v);
            return nullptr;
        }
        for (const toml::Value &item : v->array()) {
            if (!item.isTable()) {
                fail(where + key + " should hold tables, not " + toml::typeName(item.type()));
                return nullptr;
            }
        }
        return &v->array();
    }

private:
    bool wrong(const std::string &where, const char *key, const char *expected, const toml::Value &got)
    {
        return fail(where + key + " should be " + expected + ", not " + toml::typeName(got.type()));
    }

    std::string &error_;
};

std::string at(const char *what, size_t index)
{
    return std::string(what) + " " + std::to_string(index + 1) + ": ";
}

// A track's or clip's id: optional (missing keeps kNoId); anything but a whole number from 0 to
// 4294967295 is refused
bool readId(FieldReader &r, const toml::Value &table, const std::string &where, ObjectId &out)
{
    const toml::Value *v = table.at("id");
    if (!v)
        return true;
    if (v->type() != toml::Type::Integer || v->integer() < 0 || v->integer() > 4294967295LL)
        return r.fail(where + "id must be a whole number from 0 to 4294967295");
    out = static_cast<ObjectId>(v->integer());
    return true;
}

// Reads what writeAutomation wrote under `table` into `out`. A track's lanes (`track`) are "volume"
// and "pan"; a device's are parameter ids. Values are clamped to 0-1, points sorted, a parameter's
// second lane dropped. The master's (`master`, with `track`) is "volume" alone.
bool readAutomation(FieldReader &r, const toml::Value &table, bool track, const std::string &where,
                    std::vector<audio::AutomationLane> &out, bool master = false, const char *key = "automation")
{
    const toml::Array *lanes = r.tables(table, key, where);
    if (!r.ok())
        return false;
    for (size_t li = 0; lanes && li < lanes->size(); ++li) {
        const toml::Value &lv = (*lanes)[li];
        const std::string laneWhere = where + at(key, li);
        audio::AutomationLane lane;
        const toml::Value *param = lv.at("param");
        // The master has a volume and nothing else of its own to automate here
        if (master && param && param->type() == toml::Type::String && param->string() != "volume")
            return r.fail(laneWhere + "the master's only lane is \"volume\"");
        if (track && param && param->type() == toml::Type::String && param->string() == "volume")
            lane.param = audio::kVolumeParam;
        else if (track && param && param->type() == toml::Type::String && param->string() == "pan")
            lane.param = audio::kPanParam;
        else if (track && param && param->type() == toml::Type::String && param->string() == "send") {
            // A send's level: the return it goes to by id (the send itself is checked with the routing)
            const toml::Value *to = lv.at("to");
            if (!to || to->type() != toml::Type::Integer || to->integer() <= 0 ||
                !isSendParam(sendParam(static_cast<ObjectId>(std::min<int64_t>(to->integer(), UINT32_MAX)))))
                return r.fail(laneWhere + "a send's lane needs the return's or group's id as to");
            lane.param = sendParam(static_cast<ObjectId>(to->integer()));
        } else if (!track && param && param->type() == toml::Type::Integer && param->integer() >= 0 &&
                 param->integer() <= static_cast<int64_t>(UINT32_MAX))
            lane.param = static_cast<uint32_t>(param->integer());
        else
            return r.fail(laneWhere + (track ? "param should be \"volume\", \"pan\" or \"send\"" : "param should be a parameter id"));
        const toml::Array *points = r.tables(lv, "points", laneWhere);
        if (!r.ok())
            return false;
        for (size_t pi = 0; points && pi < points->size(); ++pi) {
            audio::AutomationPoint point;
            r.real((*points)[pi], "beat", laneWhere + at("point", pi), point.beat);
            r.real((*points)[pi], "value", laneWhere + at("point", pi), point.value);
            if (!r.ok())
                return false;
            lane.points.push_back({std::max(0.0, point.beat), std::clamp(point.value, 0.0f, 1.0f)});
        }
        std::stable_sort(lane.points.begin(), lane.points.end(),
                         [](const audio::AutomationPoint &a, const audio::AutomationPoint &b) { return a.beat < b.beat; });
        if (!audio::findAutomation(out, lane.param))
            out.push_back(std::move(lane));
    }
    return true;
}

// Reads what writeDevice wrote into `device`. `instrument` says which kinds may stand there (an
// instrument: a sampler, a synth or a plugin; an effect: a built-in one or a plugin). Unknown kinds
// and built-in effects are refused; a built-in effect's, a sampler's or a synth's values are clamped
// into range (unknown ones dropped, missing ones at their defaults); a plugin needn't be installed.
bool readDevice(FieldReader &r, const toml::Value &table, bool instrument, const std::string &where, audio::DeviceSetting &device)
{
    std::string kind, state;
    audio::DeviceSetting d;
    r.text(table, "kind", where, kind);
    r.text(table, "id", where, d.id);
    r.text(table, "name", where, d.name);
    r.flag(table, "bypass", where, d.bypass);
    r.text(table, "state", where, state);
    if (const toml::Value *key = table.at("sidechain")) {
        if (key->type() != toml::Type::Integer || key->integer() < 0 || key->integer() > 4294967295LL)
            return r.fail(where + "sidechain must be a track's id");
        d.sidechain = static_cast<uint32_t>(key->integer());
    }
    const toml::Array *params = r.tables(table, "params", where);
    if (!r.ok())
        return false;
    if (!audio::deviceKindFromKey(kind, d.kind))
        return r.fail(where + "unknown kind of device \"" + kind + "\"");
    if (instrument && d.kind == audio::DeviceKind::Effect)
        return r.fail(where + "a built-in effect can't be an instrument");
    if (!instrument && d.kind == audio::DeviceKind::Sampler)
        return r.fail(where + "a sampler can't be an effect");
    if (!instrument && d.kind == audio::DeviceKind::Synth)
        return r.fail(where + "a synth can't be an effect");
    if (d.id.empty())
        return r.fail(where + "a device needs its id");
    if (!state.empty()) {
        std::string bytes;
        if (!fromBase64(state, bytes))
            return r.fail(where + "the device's state isn't base64");
        d.state = std::make_shared<const std::string>(std::move(bytes));
    }
    plugin::ParamValues read;
    for (size_t pi = 0; params && pi < params->size(); ++pi) {
        const toml::Value &pv = (*params)[pi];
        const std::string paramWhere = where + at("parameter", pi);
        int id = -1;
        double value = 0.0;
        r.whole(pv, "id", paramWhere, id);
        r.real(pv, "value", paramWhere, value);
        if (!r.ok())
            return false;
        if (id < 0)
            return r.fail(paramWhere + "a parameter needs its id");
        plugin::setValue(read, static_cast<uint32_t>(id), value);
    }
    if (d.kind == audio::DeviceKind::Plugin) {
        d.values = std::move(read);
    } else {
        // The device's own parameters, from their defaults
        audio::DeviceSetting defaults;
        std::vector<audio::DeviceParam> known;
        if (d.kind == audio::DeviceKind::Effect) {
            audio::EffectType type;
            if (!audio::effectFromKey(d.id, type))
                return r.fail(where + "unknown effect \"" + d.id + "\"");
            defaults = audio::effectSetting(type);
            known = audio::makeEffect(type)->params();
        } else if (d.kind == audio::DeviceKind::Synth) {
            if (d.id != audio::SynthDevice::kId)
                return r.fail(where + "unknown synth \"" + d.id + "\"");
            defaults = audio::SynthDevice::setting();
            known = audio::synth::synthParams();
        } else {
            defaults = audio::SamplerDevice::setting(d.id);
            known = audio::SamplerDevice(defaults).params();
        }
        d.values = defaults.values;
        for (const audio::DeviceParam &param : known)
            d.setValue(param.id, std::clamp(plugin::valueOf(read, param.id, param.def), param.min, param.max));
        if (d.name.empty())
            d.name = defaults.name;
    }
    if (d.name.empty())
        d.name = d.id;
    if (!readAutomation(r, table, false, where, d.automation))
        return false;
    device = std::move(d);
    return true;
}

// Reads the effects under `key`
bool readEffects(FieldReader &r, const toml::Value &table, const char *key, const std::string &where,
                 std::vector<audio::DeviceSetting> &out)
{
    const toml::Array *effects = r.tables(table, key, where);
    if (!r.ok())
        return false;
    for (size_t i = 0; effects && i < effects->size() && out.size() < audio::kMaxEffects; ++i) {
        audio::DeviceSetting d;
        if (!readDevice(r, (*effects)[i], false, where + at("effect", i), d))
            return false;
        out.push_back(std::move(d));
    }
    return true;
}

}

namespace {

// An enum by its name in the file; a missing key keeps what `out` holds, an unknown name is refused
template <typename E>
bool readEnum(FieldReader &r, const toml::Value &table, const char *key, const std::string &where, E &out, const char *what)
{
    std::string text = enumName(out);
    if (!r.text(table, key, where, text))
        return false;
    if (!fromName(text, out))
        return r.fail(where + "unknown " + what + " \"" + text + "\"");
    return true;
}

// One export target. Values out of range are clamped (the bit depth and sample rate fall back to
// their usual ones); unknown keys are ignored.
bool readExportTarget(FieldReader &r, const toml::Value &table, const std::string &where, ExportTarget &t)
{
    r.flag(table, "enabled", where, t.enabled);
    r.text(table, "name", where, t.name);
    if (const toml::Value *tracks = table.at("tracks")) {
        if (!tracks->isArray())
            return r.fail(where + "tracks should be an array, not " + toml::typeName(tracks->type()));
        std::vector<std::string> names;
        for (const toml::Value &item : tracks->array()) {
            if (item.type() != toml::Type::String)
                return r.fail(where + "tracks should hold track names, not " + toml::typeName(item.type()));
            names.push_back(item.string());
        }
        t.tracks = std::move(names);
    }
    if (!readEnum(r, table, "source", where, t.source, "export source") ||
        !readEnum(r, table, "tap", where, t.tap, "export tap") ||
        !readEnum(r, table, "range", where, t.range, "export range") ||
        !readEnum(r, table, "format", where, t.format, "export format") ||
        !readEnum(r, table, "normalize", where, t.normalize, "normalize mode") ||
        !readEnum(r, table, "dither", where, t.dither, "dither mode"))
        return false;
    r.flag(table, "respect_mute_solo", where, t.respectMuteSolo);
    r.real(table, "from_bar", where, t.fromBar);
    r.real(table, "to_bar", where, t.toBar);
    r.real(table, "tail_seconds", where, t.tailSeconds);
    r.whole(table, "bits", where, t.bits);
    r.whole(table, "bitrate", where, t.bitrate);
    int rate = static_cast<int>(t.sampleRate);
    r.whole(table, "sample_rate", where, rate);
    r.flag(table, "mono", where, t.mono);
    r.real(table, "peak_db", where, t.peakDb);
    r.real(table, "loudness_lufs", where, t.loudnessLufs);
    r.flag(table, "limiter", where, t.limiter);
    r.real(table, "ceiling_dbtp", where, t.ceilingDbtp);
    r.real(table, "fade_in_ms", where, t.fadeInMs);
    r.real(table, "fade_out_ms", where, t.fadeOutMs);
    r.text(table, "file_name", where, t.fileName);
    r.text(table, "folder", where, t.folder);
    if (!r.ok())
        return false;

    t.bits = (t.bits == 16 || t.bits == 24 || t.bits == 32) ? t.bits : 24;
    const int rates[] = {22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
    const bool knownRate = std::find(std::begin(rates), std::end(rates), rate) != std::end(rates);
    t.sampleRate = knownRate ? static_cast<uint32_t>(rate) : 48000u;
    if (std::find(std::begin(audio::kMp3Bitrates), std::end(audio::kMp3Bitrates), t.bitrate) == std::end(audio::kMp3Bitrates))
        t.bitrate = 320;
    // An MP3 is encoded from 16 bits at 32, 44.1 or 48 kHz
    if (t.format == ExportFormat::Mp3) {
        t.sampleRate = mp3RateFor(t.sampleRate);
        t.bits = 16;
    }
    t.tailSeconds = std::clamp(t.tailSeconds, 0.0, 30.0);
    t.loudnessLufs = std::clamp(t.loudnessLufs, -40.0, 0.0);
    t.peakDb = std::clamp(t.peakDb, -20.0, 0.0);
    t.ceilingDbtp = std::clamp(t.ceilingDbtp, -20.0, 0.0);
    t.fadeInMs = std::clamp(t.fadeInMs, 0.0, 60000.0);
    t.fadeOutMs = std::clamp(t.fadeOutMs, 0.0, 60000.0);
    t.fromBar = std::max(1.0, t.fromBar);
    t.toBar = std::max(t.toBar, t.fromBar + 1.0);
    return true;
}

// One preset: its name and its [[export_presets.targets]] tables
bool readExportPreset(FieldReader &r, const toml::Value &table, const std::string &where, ExportPreset &preset)
{
    r.text(table, "name", where, preset.name);
    const toml::Array *targets = r.tables(table, "targets", where);
    if (!r.ok())
        return false;
    for (size_t i = 0; targets && i < targets->size(); ++i) {
        ExportTarget target;
        if (!readExportTarget(r, (*targets)[i], where + at("target", i), target))
            return false;
        preset.targets.push_back(std::move(target));
    }
    return r.ok();
}

}

bool readProject(const std::string &text, Project &project, std::string &error, const std::string &folder)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return false;
    error.clear();
    FieldReader r(error);

    int format = kProjectFormat;
    if (!r.whole(root, "format", "", format))
        return false;
    if (format > kProjectFormat)
        return r.fail("the project was saved by a newer version of VYBN (format " + std::to_string(format) + ")");
    if (format < kProjectFormat)
        return r.fail("the project was saved by an older version of VYBN (format " + std::to_string(format) +
                      "), which this one no longer opens");

    Project p;
    p.tracks.clear();
    r.text(root, "name", "", p.name);
    r.real(root, "tempo", "", p.tempo);
    r.whole(root, "beats_per_bar", "", p.beatsPerBar);
    r.whole(root, "bars", "", p.bars);
    r.real(root, "loop_start", "", p.loopStart);
    r.real(root, "loop_end", "", p.loopEnd);
    r.flag(root, "looping", "", p.looping);
    r.real(root, "master_volume", "", p.masterVolume);
    if (!r.ok())
        return false;
    p.masterVolume = std::clamp(p.masterVolume, 0.0f, 2.0f);
    if (!readAutomation(r, root, true, "", p.masterAutomation, true, "master_automation"))
        return false;
    p.loopStart = std::max(0.0, p.loopStart);
    p.loopEnd = std::max(p.loopStart, p.loopEnd);
    p.looping = p.looping && p.hasLoop();
    p.tempo = std::clamp(p.tempo, 20.0, 400.0);
    p.beatsPerBar = std::clamp(p.beatsPerBar, 1, 16);
    p.bars = std::clamp(p.bars, 1, 9999);

    const toml::Array *tracks = r.tables(root, "tracks", "");
    if (!r.ok())
        return false;
    for (size_t ti = 0; tracks && ti < tracks->size(); ++ti) {
        const toml::Value &tv = (*tracks)[ti];
        const std::string where = at("track", ti);
        Track t;
        std::string kind = "audio";
        if (!readId(r, tv, where, t.id))
            return false;
        r.text(tv, "name", where, t.name);
        r.text(tv, "kind", where, kind);
        if (!r.ok())
            return false;
        if (kind == "instrument")
            t.kind = TrackKind::Instrument;
        else if (kind == "return")
            t.kind = TrackKind::Return;
        else if (kind == "group")
            t.kind = TrackKind::Group;
        else if (kind != "audio")
            return r.fail(where + "unknown kind \"" + kind + "\"");
        if (const toml::Value *instrument = tv.at("instrument"); instrument && t.kind == TrackKind::Instrument) {
            if (!instrument->isTable())
                return r.fail(where + "instrument should be a table");
            if (!readDevice(r, *instrument, true, where + "instrument: ", t.instrument))
                return false;
        }
        r.real(tv, "gain", where, t.gain);
        r.real(tv, "pan", where, t.pan);
        r.flag(tv, "mute", where, t.mute);
        r.flag(tv, "solo", where, t.solo);
        r.flag(tv, "armed", where, t.armed);
        r.flag(tv, "folded", where, t.folded);
        r.whole(tv, "input_channel", where, t.inputChannel);
        r.flag(tv, "input_stereo", where, t.inputStereo);
        r.flag(tv, "monitor", where, t.monitor);
        if (!r.ok())
            return false;
        t.inputChannel = std::clamp(t.inputChannel, -1, 63);
        t.gain = std::clamp(t.gain, 0.0f, 2.0f);
        t.pan = std::clamp(t.pan, -1.0f, 1.0f);
        t.armed = t.armed && !t.bus(); // buses record nothing
        t.folded = t.folded && t.kind == TrackKind::Group; // only groups fold
        // Its output (a group's id) and sends (each to a return's or group's id), checked once every track is read
        if (const toml::Value *output = tv.at("output")) {
            if (output->type() != toml::Type::Integer || output->integer() < 0 || output->integer() > 4294967295LL)
                return r.fail(where + "output must be a track's id");
            t.output = static_cast<ObjectId>(output->integer());
        }
        const toml::Array *sends = r.tables(tv, "sends", where);
        if (!r.ok())
            return false;
        for (size_t si = 0; sends && si < sends->size(); ++si) {
            const toml::Value &sv = (*sends)[si];
            const std::string sendWhere = where + at("send", si);
            Send send;
            const toml::Value *to = sv.at("to");
            if (!to || to->type() != toml::Type::Integer || to->integer() <= 0 || to->integer() > 4294967295LL)
                return r.fail(sendWhere + "to must be a return or group track's id");
            send.to = static_cast<ObjectId>(to->integer());
            r.real(sv, "level", sendWhere, send.level);
            r.flag(sv, "pre_fader", sendWhere, send.preFader);
            if (!r.ok())
                return false;
            send.level = std::clamp(send.level, 0.0f, 1.0f);
            t.sends.push_back(send);
        }
        if (!readAutomation(r, tv, true, where, t.automation))
            return false;

        if (!readEffects(r, tv, "effects", where, t.effects))
            return false;
        const toml::Array *clips = r.tables(tv, "clips", where);
        if (!r.ok())
            return false;
        for (size_t ci = 0; clips && !t.bus() && ci < clips->size(); ++ci) { // buses hold no clips
            const toml::Value &cv = (*clips)[ci];
            const std::string clipWhere = where + at("clip", ci);
            Clip c;
            if (!readId(r, cv, clipWhere, c.id))
                return false;
            r.text(cv, "name", clipWhere, c.name);
            r.real(cv, "start", clipWhere, c.start);
            r.real(cv, "length", clipWhere, c.length);
            if (t.kind == TrackKind::Audio) {
                r.text(cv, "audio_file", clipWhere, c.audioFile);
                r.real(cv, "offset", clipWhere, c.offset);
                r.real(cv, "gain", clipWhere, c.gain);
                r.real(cv, "fade_in", clipWhere, c.fadeIn);
                r.real(cv, "fade_out", clipWhere, c.fadeOut);
            }
            if (!r.ok())
                return false;
            c.start = std::max(0.0, c.start);
            c.length = std::max(1.0 / 64.0, c.length);
            c.audioFile = resolve(c.audioFile, folder);
            c.offset = std::max(0.0, c.offset);
            c.gain = std::clamp(c.gain, 0.0f, 4.0f);
            c.fadeIn = std::max(0.0, c.fadeIn);
            c.fadeOut = std::max(0.0, c.fadeOut);
            const toml::Array *notes = r.tables(cv, "notes", clipWhere);
            if (!r.ok())
                return false;
            for (size_t ni = 0; notes && t.kind == TrackKind::Instrument && ni < notes->size(); ++ni) {
                const toml::Value &nv = (*notes)[ni];
                const std::string noteWhere = clipWhere + at("note", ni);
                Note n;
                r.real(nv, "start", noteWhere, n.start);
                r.real(nv, "length", noteWhere, n.length);
                r.whole(nv, "key", noteWhere, n.key);
                r.real(nv, "velocity", noteWhere, n.velocity);
                r.flag(nv, "muted", noteWhere, n.muted);
                if (!r.ok())
                    return false;
                n.start = std::max(0.0, n.start);
                n.length = std::max(1.0 / 64.0, n.length);
                n.key = std::clamp(n.key, 0, 127);
                n.velocity = std::clamp(n.velocity, 0.0f, 1.0f);
                c.notes.push_back(n);
            }
            // Controller lanes: values clamped, points sorted, one lane per controller
            const toml::Array *controls = r.tables(cv, "controls", clipWhere);
            if (!r.ok())
                return false;
            for (size_t li = 0; controls && t.kind == TrackKind::Instrument && li < controls->size(); ++li) {
                const toml::Value &lv = (*controls)[li];
                const std::string laneWhere = clipWhere + at("controls", li);
                ControlLane lane;
                r.whole(lv, "controller", laneWhere, lane.controller);
                const toml::Array *points = r.tables(lv, "points", laneWhere);
                if (!r.ok())
                    return false;
                if (lane.controller < 0 || lane.controller > kPitchBend) {
                    error = laneWhere + "controller " + std::to_string(lane.controller) + " isn't 0-127 or 128 (the pitch bend)";
                    return false;
                }
                for (size_t pi = 0; points && pi < points->size(); ++pi) {
                    ControlPoint point;
                    r.real((*points)[pi], "beat", laneWhere + at("point", pi), point.beat);
                    r.real((*points)[pi], "value", laneWhere + at("point", pi), point.value);
                    if (!r.ok())
                        return false;
                    lane.points.push_back({std::max(0.0, point.beat), std::clamp(point.value, 0.0f, 1.0f)});
                }
                std::stable_sort(lane.points.begin(), lane.points.end(),
                                 [](const ControlPoint &a, const ControlPoint &b) { return a.beat < b.beat; });
                const bool taken = std::any_of(c.controls.begin(), c.controls.end(),
                                               [&lane](const ControlLane &l) { return l.controller == lane.controller; });
                if (!taken)
                    c.controls.push_back(std::move(lane));
            }
            t.clips.push_back(std::move(c));
        }
        p.tracks.push_back(std::move(t));
    }
    if (!readEffects(r, root, "master_effects", "", p.masterEffects))
        return false;
    // The routing, now that every track and device is there: each output into a group, each send into
    // a return or a group, each sidechain from a track on an effect that takes one, nothing coming back to where
    // it started
    if (std::string wrong = routingError(p); !wrong.empty())
        return r.fail(wrong);
    // A file with no presets at all (no export_presets key) gets the defaults
    if (!root.at("export_presets")) {
        p.exportPresets = defaultExportPresets();
    } else {
        const toml::Array *presets = r.tables(root, "export_presets", "");
        if (!r.ok())
            return false;
        for (size_t i = 0; presets && i < presets->size(); ++i) {
            ExportPreset preset;
            if (!readExportPreset(r, (*presets)[i], at("preset", i), preset))
                return false;
            p.exportPresets.push_back(std::move(preset));
        }
    }
    if (const toml::Value *meta = root.at("export_metadata"); meta) {
        if (!meta->isTable())
            return r.fail("export_metadata should be a table, not " + std::string(toml::typeName(meta->type())));
        r.text(*meta, "title", "export_metadata: ", p.exportMetadata.title);
        r.text(*meta, "artist", "export_metadata: ", p.exportMetadata.artist);
        r.text(*meta, "album", "export_metadata: ", p.exportMetadata.album);
        r.text(*meta, "comment", "export_metadata: ", p.exportMetadata.comment);
        if (!r.ok())
            return false;
    }
    // Files without ids (or with duplicates) come back with unique ones
    ObjectId next = 1;
    p.normalizeIds(next);
    project = std::move(p);
    return true;
}

bool saveProjectFile(const std::string &path, const Project &project, std::string &error)
{
    const fs::path target(path);
    const fs::path temporary = fs::path(path + ".saving");
    std::error_code ec;
    if (target.has_parent_path())
        fs::create_directories(target.parent_path(), ec);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Couldn't write " + temporary.string();
            return false;
        }
        const std::string text = writeProject(project, target.parent_path().string());
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            error = "Couldn't write " + temporary.string();
            fs::remove(temporary, ec);
            return false;
        }
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        error = "Couldn't replace " + path + ": " + ec.message();
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool loadProjectFile(const std::string &path, Project &project, std::string &error)
{
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        error = "Couldn't open " + path;
        return false;
    }
    std::ostringstream text;
    text << in.rdbuf();
    if (!readProject(text.str(), project, error, fs::path(path).parent_path().string())) {
        error = fs::path(path).filename().string() + ", " + error;
        return false;
    }
    return true;
}

}
