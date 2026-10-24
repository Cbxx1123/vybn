// vybn-test-plugins.vst3: the VST3 plugins the plugin self-test loads through the sandbox. Written
// against Steinberg's interface headers alone (no SDK helper classes), so it also shows the least
// a VST3 plugin has to do.
//
// - VST Gain (Fx): a stereo gain, its component and edit controller one object. Parameters: Gain
//   (0-1, shown as -60 to +12 dB), Mute (one step), Peak (read-only: each block's output peak goes
//   back as an output parameter change), Latency (eight steps of 512 frames: it delays its sound by
//   that much and asks the host to restart it, VST3's way of saying its latency changed). Saves
//   Gain, Mute and Latency. Its editor is a window of its own drawn grey: a click on its left half
//   takes the gain down a step, on its right half up, through the host as an edit would, and says
//   its state is dirty (IComponentHandler2).
// - VST Sine (Instrument|Synth): a sine voice on its event input; Level, and Bend, which the pitch
//   bend drives through IMidiMapping (plus Mod, driven by controller 1, reported but unused), and
//   Program, a choice of three programs from the root unit's program list. Level is in the unit
//   "Voice", Bend in "Pitch" under it (IUnitInfo). A note-off ends its note by the note's id. It
//   lists the tuning note expression (INoteExpressionController) and bends its note by it, by id.
// - VST Duck (Fx): a stereo effect with a stereo aux input (its sidechain): its output is its input
//   times (1 - the key's peak in the block). Its edit controller is a separate class, and it
//   processes 64-bit audio only.
// - Test Gain (Fx): the same as VST Gain under the CLAP test plugin's name and vendor, so the
//   catalogue's choice between two formats of one plugin can be seen.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr double kPi = 3.14159265358979323846;

bool same(const TUID a, const TUID b)
{
    return std::memcmp(a, b, sizeof(TUID)) == 0;
}

void copy(String128 out, const char *text)
{
    int i = 0;
    for (; text[i] && i < 127; ++i)
        out[i] = static_cast<TChar>(text[i]);
    out[i] = 0;
}

void copyAscii(char8 *out, size_t size, const char *text)
{
    std::strncpy(out, text, size - 1);
    out[size - 1] = 0;
}

// The class ids: a pattern with the class's number in its last byte
#define TEST_CID(n) INLINE_UID(0x56594241, 0x4E544553, 0x54564633, 0x00000000 + (n))
const TUID kGainId = TEST_CID(1);
const TUID kSineId = TEST_CID(2);
const TUID kDuckId = TEST_CID(3);
const TUID kDuckControllerId = TEST_CID(4);
const TUID kTwinId = TEST_CID(5);

enum Kind { Gain, Sine, Duck, DuckController, Twin };

// Reads the first parameter queue's last point for a parameter in a block, if it has one
bool lastPoint(IParameterChanges *changes, ParamID id, ParamValue &value)
{
    if (!changes)
        return false;
    bool found = false;
    for (int32 i = 0; i < changes->getParameterCount(); ++i) {
        IParamValueQueue *q = changes->getParameterData(i);
        if (!q || q->getParameterId() != id || q->getPointCount() <= 0)
            continue;
        int32 offset = 0;
        if (q->getPoint(q->getPointCount() - 1, offset, value) == kResultOk)
            found = true;
    }
    return found;
}

class Editor;

// One plugin: a component that is (for every kind but Duck) its own edit controller too
class Plugin : public IComponent,
               public IAudioProcessor,
               public IEditController,
               public IMidiMapping,
               public IUnitInfo,
               public INoteExpressionController {
public:
    explicit Plugin(Kind kind) : kind_(kind) {}
    virtual ~Plugin() = default;

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        const bool controller = kind_ != Duck;
        const bool component = kind_ != DuckController;
        if (same(iid, FUnknown_iid) || same(iid, IPluginBase_iid)) {
            *obj = component ? static_cast<void *>(static_cast<IComponent *>(this)) : static_cast<void *>(static_cast<IEditController *>(this));
        } else if (component && same(iid, IComponent_iid)) {
            *obj = static_cast<IComponent *>(this);
        } else if (component && same(iid, IAudioProcessor_iid)) {
            *obj = static_cast<IAudioProcessor *>(this);
        } else if (controller && same(iid, IEditController_iid)) {
            *obj = static_cast<IEditController *>(this);
        } else if (kind_ == Sine && same(iid, IMidiMapping_iid)) {
            *obj = static_cast<IMidiMapping *>(this);
        } else if (kind_ == Sine && same(iid, IUnitInfo_iid)) {
            *obj = static_cast<IUnitInfo *>(this);
        } else if (kind_ == Sine && same(iid, INoteExpressionController_iid)) {
            *obj = static_cast<INoteExpressionController *>(this);
        } else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override
    {
        const uint32 left = --refs_;
        if (left == 0)
            delete this;
        return left;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown *) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID classId) override
    {
        if (kind_ != Duck)
            return kNotImplemented;
        std::memcpy(classId, kDuckControllerId, sizeof(TUID));
        return kResultOk;
    }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override
    {
        if (type == kAudio)
            return dir == kOutput ? 1 : (kind_ == Sine ? 0 : kind_ == Duck ? 2 : 1);
        return type == kEvent && dir == kInput && kind_ == Sine ? 1 : 0;
    }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo &bus) override
    {
        if (index < 0 || index >= getBusCount(type, dir))
            return kInvalidArgument;
        bus.mediaType = type;
        bus.direction = dir;
        bus.channelCount = type == kEvent ? 16 : 2;
        copy(bus.name, type == kEvent ? "Notes" : index == 0 ? "Main" : "Key");
        bus.busType = index == 0 ? kMain : kAux;
        bus.flags = index == 0 ? BusInfo::kDefaultActive : 0;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream *state) override
    {
        float values[3] = {};
        int32 got = 0;
        if (!state || state->read(values, sizeof values, &got) != kResultOk || got != sizeof values)
            return kResultFalse;
        gain_ = values[0];
        mute_ = values[1];
        latencyParam_ = values[2];
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream *state) override
    {
        float values[3] = {static_cast<float>(gain_), static_cast<float>(mute_), static_cast<float>(latencyParam_)};
        int32 put = 0;
        return state && state->write(values, sizeof values, &put) == kResultOk ? kResultOk : kResultFalse;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement *, int32, SpeakerArrangement *, int32) override { return kResultOk; }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32, SpeakerArrangement &arr) override
    {
        arr = SpeakerArr::kStereo;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override { return size == (kind_ == Duck ? kSample64 : kSample32) ? kResultTrue : kResultFalse; }
    uint32 PLUGIN_API getLatencySamples() override { return latencyFrames(); }
    tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override
    {
        rate_ = setup.sampleRate > 0.0 ? setup.sampleRate : 48000.0;
        delay_.assign(2, std::vector<float>(8192, 0.0f));
        return kResultOk;
    }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    uint32 PLUGIN_API getTailSamples() override { return 0; }
    tresult PLUGIN_API process(ProcessData &data) override
    {
        ParamValue value = 0.0;
        if (lastPoint(data.inputParameterChanges, 0, value))
            (kind_ == Sine ? level_ : gain_) = value;
        if (lastPoint(data.inputParameterChanges, 1, value))
            (kind_ == Sine ? bend_ : mute_) = value;
        if (kind_ == Sine && lastPoint(data.inputParameterChanges, 2, value))
            mod_ = value;
        if (kind_ == Sine && lastPoint(data.inputParameterChanges, 3, value))
            program_ = value;
        if (data.numOutputs < 1 || data.outputs[0].numChannels < 1)
            return kResultOk;
        if (kind_ == Duck) {
            // Doubles only: its output is its input times (1 - the key's peak)
            double **out = data.outputs[0].channelBuffers64;
            double **in = data.numInputs > 0 ? data.inputs[0].channelBuffers64 : nullptr;
            const int32 inChannels = data.numInputs > 0 ? data.inputs[0].numChannels : 0;
            double peak = 0.0;
            if (data.numInputs > 1) {
                for (int32 c = 0; c < data.inputs[1].numChannels; ++c)
                    for (int32 f = 0; f < data.numSamples; ++f)
                        peak = std::max(peak, std::fabs(data.inputs[1].channelBuffers64[c][f]));
            }
            for (int32 c = 0; c < data.outputs[0].numChannels; ++c)
                for (int32 f = 0; f < data.numSamples; ++f)
                    out[c][f] = (inChannels > 0 ? in[std::min(c, inChannels - 1)][f] : 0.0) * (1.0 - std::min(1.0, peak));
            return kResultOk;
        }
        float **out = data.outputs[0].channelBuffers32;
        const int32 channels = data.outputs[0].numChannels;
        const int32 frames = data.numSamples;
        if (kind_ == Sine) {
            // Notes start and end at their frames
            const int32 count = data.inputEvents ? data.inputEvents->getEventCount() : 0;
            int32 next = 0;
            Event e{};
            for (int32 f = 0; f < frames; ++f) {
                for (; next < count && data.inputEvents->getEvent(next, e) == kResultOk && e.sampleOffset <= f; ++next) {
                    if (e.type == Event::kNoteOnEvent && e.noteOn.velocity > 0.0f) {
                        key_ = e.noteOn.pitch;
                        noteId_ = e.noteOn.noteId;
                        tuning_ = 0.0;
                        on_ = true;
                    } else if (e.type == Event::kNoteExpressionValueEvent && e.noteExpressionValue.typeId == kTuningTypeID &&
                               e.noteExpressionValue.noteId == noteId_) {
                        tuning_ = 240.0 * (e.noteExpressionValue.value - 0.5);
                    } else if ((e.type == Event::kNoteOffEvent && (e.noteOff.noteId != -1 ? e.noteOff.noteId == noteId_ : e.noteOff.pitch == key_)) ||
                               (e.type == Event::kNoteOnEvent && e.noteOn.velocity == 0.0f && e.noteOn.pitch == key_)) {
                        on_ = false;
                    }
                }
                float s = 0.0f;
                if (on_) {
                    // Bend: 0.5 is centre, two semitones each way
                    const double hz = 440.0 * std::pow(2.0, (key_ - 69 + (bend_ - 0.5) * 4.0 + tuning_) / 12.0);
                    phase_ += hz / rate_;
                    phase_ -= std::floor(phase_);
                    s = static_cast<float>(0.5 * level_ * std::sin(2.0 * kPi * phase_));
                }
                for (int32 c = 0; c < channels; ++c)
                    out[c][f] = s;
            }
            return kResultOk;
        }
        float **in = data.numInputs > 0 ? data.inputs[0].channelBuffers32 : nullptr;
        const int32 inChannels = data.numInputs > 0 ? data.inputs[0].numChannels : 0;
        // Gain (and its twin): delayed by its latency, then the gain
        const double db = -60.0 + gain_ * 72.0;
        const float g = mute_ >= 0.5 ? 0.0f : static_cast<float>(std::pow(10.0, db / 20.0));
        const uint32 latency = latencyFrames();
        float peak = 0.0f;
        for (int32 c = 0; c < channels && c < 2; ++c) {
            std::vector<float> &line = delay_[static_cast<size_t>(c)];
            for (int32 f = 0; f < frames; ++f) {
                const float x = inChannels > 0 ? in[std::min(c, inChannels - 1)][f] : 0.0f;
                line[write_ % line.size()] = x;
                const float delayed = line[(write_ + line.size() - latency) % line.size()];
                if (c == channels - 1 || c == 1)
                    ++write_;
                out[c][f] = delayed * g;
                peak = std::max(peak, std::fabs(out[c][f]));
            }
            if (c == 0 && channels > 1)
                write_ -= static_cast<size_t>(frames);
        }
        if (channels == 1)
            write_ += static_cast<size_t>(frames);
        // The peak goes back as a parameter change
        if (data.outputParameterChanges) {
            int32 index = 0;
            if (IParamValueQueue *q = data.outputParameterChanges->addParameterData(2, index)) {
                int32 at = 0;
                q->addPoint(0, std::min(1.0f, peak), at);
            }
        }
        return kResultOk;
    }

    // IEditController
    tresult PLUGIN_API setComponentState(IBStream *state) override { return setState(state); }
    int32 PLUGIN_API getParameterCount() override { return kind_ == Gain || kind_ == Twin || kind_ == Sine ? 4 : 3; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override
    {
        if (index < 0 || index >= getParameterCount())
            return kInvalidArgument;
        info = ParameterInfo{};
        info.id = static_cast<ParamID>(index);
        info.flags = ParameterInfo::kCanAutomate;
        info.unitId = 0; // the root unit
        if (kind_ == Sine) {
            const char *names[] = {"Level", "Bend", "Mod", "Program"};
            const UnitID units[] = {1, 2, kRootUnitId, kRootUnitId};
            copy(info.title, names[index]);
            info.unitId = units[index];
            info.defaultNormalizedValue = index == 0 ? 0.8 : index == 1 ? 0.5 : 0.0;
            if (index == 3)
                info.flags |= ParameterInfo::kIsProgramChange; // its steps are left to its program list
        } else if (kind_ == Duck || kind_ == DuckController) {
            const char *names[] = {"Amount", "Release", "Hidden"};
            copy(info.title, names[index]);
            if (index == 2)
                info.flags = ParameterInfo::kIsHidden | ParameterInfo::kIsReadOnly;
        } else {
            const char *names[] = {"Gain", "Mute", "Peak", "Latency"};
            copy(info.title, names[index]);
            if (index == 0)
                copy(info.units, "dB");
            if (index == 1)
                info.stepCount = 1;
            if (index == 2)
                info.flags = ParameterInfo::kIsReadOnly;
            if (index == 3)
                info.stepCount = 8;
            info.defaultNormalizedValue = index == 0 ? 60.0 / 72.0 : 0.0;
        }
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue value, String128 text) override
    {
        char buffer[64];
        if ((kind_ == Gain || kind_ == Twin) && id == 0)
            std::snprintf(buffer, sizeof buffer, "%.1f dB", -60.0 + value * 72.0);
        else if ((kind_ == Gain || kind_ == Twin) && id == 1)
            std::snprintf(buffer, sizeof buffer, "%s", value >= 0.5 ? "Muted" : "On");
        else if ((kind_ == Gain || kind_ == Twin) && id == 3)
            std::snprintf(buffer, sizeof buffer, "%d frames", static_cast<int>(std::lround(value * 8.0)) * 512);
        else
            std::snprintf(buffer, sizeof buffer, "%.2f", value);
        copy(text, buffer);
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar *, ParamValue &) override { return kNotImplemented; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue value) override { return value; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue value) override { return value; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override
    {
        if (kind_ == Sine)
            return id == 0 ? level_ : id == 1 ? bend_ : id == 2 ? mod_ : program_;
        return id == 0 ? gain_ : id == 1 ? mute_ : id == 2 ? 0.0 : latencyParam_;
    }
    tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
    {
        if (kind_ == Sine) {
            (id == 0 ? level_ : id == 1 ? bend_ : id == 2 ? mod_ : program_) = value;
            return kResultOk;
        }
        if (id == 0)
            gain_ = value;
        else if (id == 1)
            mute_ = value;
        else if (id == 3 && value != latencyParam_) {
            // A new latency: the host restarts the plugin, VST3's way of changing it
            latencyParam_ = value;
            if (handler_)
                handler_->restartComponent(kLatencyChanged);
        }
        return kResultOk;
    }
    tresult PLUGIN_API setComponentHandler(IComponentHandler *handler) override
    {
        handler_ = handler;
        return kResultOk;
    }
    IPlugView *PLUGIN_API createView(FIDString name) override;

    // IMidiMapping (the sine): the bend to Bend, controller 1 to Mod
    tresult PLUGIN_API getMidiControllerAssignment(int32 bus, int16, CtrlNumber number, ParamID &id) override
    {
        if (bus != 0)
            return kResultFalse;
        if (number == kPitchBend) {
            id = 1;
            return kResultTrue;
        }
        if (number == kCtrlModWheel) {
            id = 2;
            return kResultTrue;
        }
        return kResultFalse;
    }

    // INoteExpressionController (the sine): tuning, on its one event bus
    int32 PLUGIN_API getNoteExpressionCount(int32 bus, int16) override { return bus == 0 ? 1 : 0; }
    tresult PLUGIN_API getNoteExpressionInfo(int32 bus, int16, int32 index, NoteExpressionTypeInfo &info) override
    {
        if (bus != 0 || index != 0)
            return kInvalidArgument;
        info = NoteExpressionTypeInfo{};
        info.typeId = kTuningTypeID;
        copy(info.title, "Tuning");
        copy(info.shortTitle, "Tune");
        copy(info.units, "st");
        info.unitId = -1;
        info.valueDesc = {0.5, 0.0, 1.0, 0};
        info.associatedParameterId = kNoParamId;
        info.flags = NoteExpressionTypeInfo::kIsBipolar;
        return kResultOk;
    }
    tresult PLUGIN_API getNoteExpressionStringByValue(int32, int16, NoteExpressionTypeID, NoteExpressionValue, String128) override
    {
        return kNotImplemented;
    }
    tresult PLUGIN_API getNoteExpressionValueByString(int32, int16, NoteExpressionTypeID, const TChar *, NoteExpressionValue &) override
    {
        return kNotImplemented;
    }

    // The editor's clicks: the gain a step down or up, through the host
    void step(int direction)
    {
        const double next = std::min(1.0, std::max(0.0, gain_ + direction / 72.0));
        gain_ = next;
        if (handler_) {
            handler_->beginEdit(0);
            handler_->performEdit(0, next);
            handler_->endEdit(0);
            void *more = nullptr;
            if (handler_->queryInterface(IComponentHandler2_iid, &more) == kResultOk && more)
                static_cast<IComponentHandler2 *>(more)->setDirty(true);
        }
    }

    // IUnitInfo (the sine): the root with its program list, "Voice" under it and "Pitch" under that
    int32 PLUGIN_API getUnitCount() override { return 3; }
    tresult PLUGIN_API getUnitInfo(int32 index, UnitInfo &info) override
    {
        if (index < 0 || index >= 3)
            return kInvalidArgument;
        const char *names[] = {"Root", "Voice", "Pitch"};
        info.id = index;
        info.parentUnitId = index == 0 ? kNoParentUnitId : index - 1;
        copy(info.name, names[index]);
        info.programListId = index == 0 ? kProgramList : kNoProgramListId;
        return kResultOk;
    }
    int32 PLUGIN_API getProgramListCount() override { return 1; }
    tresult PLUGIN_API getProgramListInfo(int32 index, ProgramListInfo &info) override
    {
        if (index != 0)
            return kInvalidArgument;
        info.id = kProgramList;
        copy(info.name, "Programs");
        info.programCount = 3;
        return kResultOk;
    }
    tresult PLUGIN_API getProgramName(ProgramListID list, int32 index, String128 name) override
    {
        const char *names[] = {"Soft", "Bright", "Hollow"};
        if (list != kProgramList || index < 0 || index >= 3)
            return kInvalidArgument;
        copy(name, names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API getProgramInfo(ProgramListID, int32, Steinberg::Vst::CString, String128) override { return kNotImplemented; }
    tresult PLUGIN_API hasProgramPitchNames(ProgramListID, int32) override { return kResultFalse; }
    tresult PLUGIN_API getProgramPitchName(ProgramListID, int32, int16, String128) override { return kNotImplemented; }
    UnitID PLUGIN_API getSelectedUnit() override { return kRootUnitId; }
    tresult PLUGIN_API selectUnit(UnitID) override { return kResultOk; }
    tresult PLUGIN_API getUnitByBus(MediaType, BusDirection, int32, int32, UnitID &) override { return kNotImplemented; }
    tresult PLUGIN_API setUnitProgramData(int32, int32, IBStream *) override { return kNotImplemented; }

private:
    static constexpr ProgramListID kProgramList = 7;
    uint32 latencyFrames() const { return static_cast<uint32>(std::lround(latencyParam_ * 8.0)) * 512u; }

    Kind kind_;
    std::atomic<uint32> refs_{1};
    IComponentHandler *handler_ = nullptr;
    double rate_ = 48000.0;
    double gain_ = 60.0 / 72.0, mute_ = 0.0, latencyParam_ = 0.0;
    double level_ = 0.8, bend_ = 0.5, mod_ = 0.0, program_ = 0.0;
    double phase_ = 0.0;
    int key_ = 69;
    int32 noteId_ = -1;
    double tuning_ = 0.0; // the note's tuning expression, in semitones
    bool on_ = false;
    std::vector<std::vector<float>> delay_ = std::vector<std::vector<float>>(2, std::vector<float>(8192, 0.0f));
    size_t write_ = 8192 * 4;
};

// The gain's editor: a grey child window; a click on its left half steps the gain down, its right half up
class Editor : public IPlugView {
public:
    explicit Editor(Plugin *plugin) : plugin_(plugin) { plugin_->addRef(); }
    virtual ~Editor()
    {
        removed();
        plugin_->release();
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (same(iid, FUnknown_iid) || same(iid, IPlugView_iid)) {
            *obj = static_cast<IPlugView *>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override
    {
        const uint32 left = --refs_;
        if (left == 0)
            delete this;
        return left;
    }
    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override
    {
        return type && std::strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API attached(void *parent, FIDString type) override
    {
        if (isPlatformTypeSupported(type) != kResultTrue || !parent)
            return kResultFalse;
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
            wc.lpszClassName = L"VybnTestVst3Editor";
            registered = RegisterClassW(&wc) != 0;
        }
        window_ = CreateWindowExW(0, L"VybnTestVst3Editor", L"", WS_CHILD | WS_VISIBLE, 0, 0, width_, height_,
                                  static_cast<HWND>(parent), nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_)
            return kResultFalse;
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(plugin_));
        return kResultOk;
    }
    tresult PLUGIN_API removed() override
    {
        if (window_)
            DestroyWindow(window_);
        window_ = nullptr;
        return kResultOk;
    }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API getSize(ViewRect *size) override
    {
        if (!size)
            return kInvalidArgument;
        *size = ViewRect(0, 0, width_, height_);
        return kResultOk;
    }
    tresult PLUGIN_API onSize(ViewRect *size) override
    {
        if (!size)
            return kInvalidArgument;
        width_ = size->getWidth();
        height_ = size->getHeight();
        if (window_)
            SetWindowPos(window_, nullptr, 0, 0, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
        return kResultOk;
    }
    tresult PLUGIN_API onFocus(TBool) override { return kResultOk; }
    tresult PLUGIN_API setFrame(IPlugFrame *frame) override
    {
        frame_ = frame;
        return kResultOk;
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect *) override { return kResultOk; }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_LBUTTONDOWN) {
            auto *plugin = reinterpret_cast<Plugin *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (plugin)
                plugin->step(static_cast<short>(LOWORD(lParam)) < rc.right / 2 ? -1 : 1);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    Plugin *plugin_;
    std::atomic<uint32> refs_{1};
    HWND window_ = nullptr;
    IPlugFrame *frame_ = nullptr;
    int32 width_ = 300, height_ = 120;
};

IPlugView *PLUGIN_API Plugin::createView(FIDString name)
{
    if ((kind_ != Gain && kind_ != Twin) || !name || std::strcmp(name, ViewType::kEditor) != 0)
        return nullptr;
    return new Editor(this);
}

struct ClassEntry {
    const TUID *cid;
    const char *category;
    const char *name;
    const char *subcategories;
    const char *vendor;
    Kind kind;
};

const ClassEntry kClasses[] = {
    {&kGainId, kVstAudioEffectClass, "VST Gain", "Fx|Dynamics", "VYBN Tests", Gain},
    {&kSineId, kVstAudioEffectClass, "VST Sine", "Instrument|Synth", "VYBN Tests", Sine},
    {&kDuckId, kVstAudioEffectClass, "VST Duck", "Fx|Dynamics", "VYBN Tests", Duck},
    {&kDuckControllerId, kVstComponentControllerClass, "VST Duck Controller", "", "VYBN Tests", DuckController},
    {&kTwinId, kVstAudioEffectClass, "Test Gain", "Fx", "VYBN", Twin},
};

class Factory : public IPluginFactory2 {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override
    {
        if (same(iid, FUnknown_iid) || same(iid, IPluginFactory_iid) || same(iid, IPluginFactory2_iid)) {
            *obj = static_cast<IPluginFactory2 *>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }  // the one factory lives as long as the module
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
    {
        if (!info)
            return kInvalidArgument;
        *info = PFactoryInfo{};
        copyAscii(info->vendor, sizeof info->vendor, "VYBN Tests");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return static_cast<int32>(sizeof kClasses / sizeof kClasses[0]); }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
    {
        if (index < 0 || index >= countClasses() || !info)
            return kInvalidArgument;
        const ClassEntry &e = kClasses[index];
        *info = PClassInfo{};
        std::memcpy(info->cid, *e.cid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        copyAscii(info->category, sizeof info->category, e.category);
        copyAscii(info->name, sizeof info->name, e.name);
        return kResultOk;
    }
    tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override
    {
        if (index < 0 || index >= countClasses() || !info)
            return kInvalidArgument;
        const ClassEntry &e = kClasses[index];
        *info = PClassInfo2{};
        std::memcpy(info->cid, *e.cid, sizeof(TUID));
        info->cardinality = PClassInfo::kManyInstances;
        copyAscii(info->category, sizeof info->category, e.category);
        copyAscii(info->name, sizeof info->name, e.name);
        copyAscii(info->subCategories, sizeof info->subCategories, e.subcategories);
        copyAscii(info->vendor, sizeof info->vendor, e.vendor);
        copyAscii(info->version, sizeof info->version, "1.0.0");
        copyAscii(info->sdkVersion, sizeof info->sdkVersion, "VST 3.8.0");
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void **obj) override
    {
        *obj = nullptr;
        for (const ClassEntry &e : kClasses) {
            if (!same(reinterpret_cast<const char *>(cid), *e.cid))
                continue;
            Plugin *p = new Plugin(e.kind);
            const tresult result = p->queryInterface(reinterpret_cast<const char *>(iid), obj);
            p->release();
            return result;
        }
        return kNoInterface;
    }
};

Factory factory;

}

extern "C" {
__declspec(dllexport) bool InitDll() { return true; }
__declspec(dllexport) bool ExitDll() { return true; }
__declspec(dllexport) IPluginFactory *GetPluginFactory()
{
    factory.addRef();
    return &factory;
}
}
