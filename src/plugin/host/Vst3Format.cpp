// The host's VST3 plugins, written against Steinberg's interface headers alone (each interface's
// id an inline constant, compared byte for byte; none of the SDK's code). A plugin is a component
// (its audio processor) and an edit controller (its parameters and interface): one object, or two
// the host connects. The app sees it as it sees a CLAP plugin: a stepped parameter runs 0 to its
// step count and any other 0 to 1; notes, the bend and controllers (through the plugin's MIDI
// mapping) arrive as events and parameter changes; its aux input is the sidechain; its state is
// both halves' in one blob of the host's; its editor is parented to the same frame window.
//
// Parameter changes travel three ways: the app's (and automation's) to the processor in the block
// and on to the controller on the main thread; the controller's edits (its interface) to the
// processor in the next block and back to the app; the processor's output changes back to the app
// and on to the controller. Only the audio thread pushes into the ring to the app while the plugin
// is active, so edits made then go through it.

#include "plugin/host/Host.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

#include <clap/clap.h>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

namespace host {

namespace {

using namespace plugin;
using namespace Steinberg;
using namespace Steinberg::Vst;

bool same(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

std::string utf8(const char16 *text, size_t size)
{
    std::wstring w;
    for (size_t i = 0; i < size && text[i]; ++i)
        w += static_cast<wchar_t>(text[i]);
    return narrow(w);
}

std::string ascii(const char8 *text, size_t size)
{
    size_t n = 0;
    while (n < size && text[n])
        ++n;
    return std::string(text, n);
}

void copy(String128 out, const std::string &text)
{
    const std::wstring w = widen(text);
    size_t i = 0;
    for (; i < w.size() && i < 127; ++i)
        out[i] = static_cast<TChar>(w[i]);
    out[i] = 0;
}

// A plugin's id: "vst3:" and its class id in hex, so it never meets a CLAP id
std::string idOf(const TUID cid)
{
    static const char digits[] = "0123456789abcdef";
    std::string out = "vst3:";
    for (size_t i = 0; i < sizeof(TUID); ++i) {
        out += digits[(static_cast<uint8_t>(cid[i]) >> 4) & 15];
        out += digits[static_cast<uint8_t>(cid[i]) & 15];
    }
    return out;
}

bool cidOf(const std::string &id, TUID cid)
{
    if (id.size() != 5 + 2 * sizeof(TUID) || id.compare(0, 5, "vst3:") != 0)
        return false;
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
    for (size_t i = 0; i < sizeof(TUID); ++i) {
        const int hi = nibble(id[5 + 2 * i]), lo = nibble(id[6 + 2 * i]);
        if (hi < 0 || lo < 0)
            return false;
        cid[i] = static_cast<char>(hi * 16 + lo);
    }
    return true;
}

// A class's subcategories ("Instrument|Synth", "Fx|Delay") as CLAP's feature words, which the app
// asks ("instrument synthesizer", "audio-effect delay")
std::string featuresOf(const std::string &subcategories)
{
    static const std::map<std::string, std::string> words = {
        {"instrument", "instrument"}, {"fx", "audio-effect"},      {"synth", "synthesizer"},   {"sampler", "sampler"},
        {"drum", "drum-machine"},     {"delay", "delay"},          {"reverb", "reverb"},       {"dynamics", "compressor"},
        {"eq", "equalizer"},          {"filter", "filter"},        {"distortion", "distortion"}, {"analyzer", "analyzer"},
        {"modulation", "modulation"}, {"pitch shift", "pitch-shifter"}, {"mastering", "mastering"}, {"restoration", "restoration"},
        {"mono", "mono"},             {"stereo", "stereo"},        {"surround", "surround"},   {"tools", "utility"},
        {"generator", "instrument"},  {"piano", "piano"},          {"external", "external"},   {"spatial", "spatial"},
    };
    std::string out;
    bool instrument = false, effect = false;
    size_t start = 0;
    while (start <= subcategories.size()) {
        size_t end = subcategories.find('|', start);
        if (end == std::string::npos)
            end = subcategories.size();
        std::string word = subcategories.substr(start, end - start);
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        start = end + 1;
        if (word.empty())
            continue;
        const auto found = words.find(word);
        std::string feature = found != words.end() ? found->second : word;
        std::replace(feature.begin(), feature.end(), ' ', '-');
        if ((" " + out + " ").find(" " + feature + " ") != std::string::npos)
            continue;
        instrument = instrument || feature == "instrument";
        effect = effect || feature == "audio-effect";
        out += (out.empty() ? "" : " ") + feature;
    }
    if (!instrument && !effect)
        out = "audio-effect" + std::string(out.empty() ? "" : " ") + out;
    return out;
}

uint32_t channelsOf(SpeakerArrangement arrangement)
{
    uint32_t n = 0;
    for (; arrangement; arrangement &= arrangement - 1)
        ++n;
    return n;
}

// --- What the host gives a plugin

// An object the host keeps for as long as it lives: counting its references doesn't matter
#define VYBN_FOREVER                                          \
    uint32 PLUGIN_API addRef() override { return 1000; }     \
    uint32 PLUGIN_API release() override { return 1000; }

// An object the plugin may keep: deleted with its last reference
#define VYBN_COUNTED                                          \
    uint32 PLUGIN_API addRef() override { return ++refs_; }  \
    uint32 PLUGIN_API release() override                      \
    {                                                         \
        const uint32 left = --refs_;                          \
        if (left == 0)                                        \
            delete this;                                      \
        return left;                                          \
    }                                                         \
    std::atomic<uint32> refs_{1};

template <class Interface>
tresult answer(Interface *self, const TUID asked, const TUID iid, void **obj)
{
    if (same(asked, FUnknown_iid) || same(asked, iid)) {
        self->addRef();
        *obj = self;
        return kResultOk;
    }
    *obj = nullptr;
    return kNoInterface;
}

class AttributeList : public IAttributeList {
public:
    virtual ~AttributeList() = default;
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IAttributeList>(this, iid, IAttributeList_iid, obj); }
    VYBN_COUNTED

    tresult PLUGIN_API setInt(AttrID id, int64 value) override { return set(id, Value{0, value, 0.0, {}}); }
    tresult PLUGIN_API getInt(AttrID id, int64 &value) override
    {
        const Value *v = get(id, 0);
        return v ? (value = v->i, kResultOk) : kResultFalse;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override { return set(id, Value{1, 0, value, {}}); }
    tresult PLUGIN_API getFloat(AttrID id, double &value) override
    {
        const Value *v = get(id, 1);
        return v ? (value = v->f, kResultOk) : kResultFalse;
    }
    tresult PLUGIN_API setString(AttrID id, const TChar *text) override
    {
        Value v{2, 0, 0.0, {}};
        for (size_t i = 0; text && text[i]; ++i)
            v.bytes.append(reinterpret_cast<const char *>(&text[i]), sizeof(TChar));
        return set(id, v);
    }
    tresult PLUGIN_API getString(AttrID id, TChar *text, uint32 sizeInBytes) override
    {
        const Value *v = get(id, 2);
        if (!v || !text || sizeInBytes < sizeof(TChar))
            return kResultFalse;
        const size_t n = std::min<size_t>(v->bytes.size(), sizeInBytes - sizeof(TChar));
        std::memcpy(text, v->bytes.data(), n);
        text[n / sizeof(TChar)] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void *data, uint32 sizeInBytes) override
    {
        return set(id, Value{3, 0, 0.0, std::string(static_cast<const char *>(data), data ? sizeInBytes : 0)});
    }
    tresult PLUGIN_API getBinary(AttrID id, const void *&data, uint32 &sizeInBytes) override
    {
        const Value *v = get(id, 3);
        if (!v)
            return kResultFalse;
        data = v->bytes.data();
        sizeInBytes = static_cast<uint32>(v->bytes.size());
        return kResultOk;
    }

private:
    struct Value {
        int kind;
        int64 i;
        double f;
        std::string bytes;
    };
    tresult set(AttrID id, const Value &v)
    {
        if (!id)
            return kInvalidArgument;
        values_[id] = v;
        return kResultOk;
    }
    const Value *get(AttrID id, int kind) const
    {
        const auto found = id ? values_.find(id) : values_.end();
        return found != values_.end() && found->second.kind == kind ? &found->second : nullptr;
    }
    std::map<std::string, Value> values_;
};

class MessageObject : public IMessage {
public:
    virtual ~MessageObject() { attributes_->release(); }
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IMessage>(this, iid, IMessage_iid, obj); }
    VYBN_COUNTED

    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id ? id : ""; }
    IAttributeList *PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    AttributeList *attributes_ = new AttributeList;
};

class HostApplication : public IHostApplication {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IHostApplication>(this, iid, IHostApplication_iid, obj); }
    VYBN_FOREVER

    tresult PLUGIN_API getName(String128 name) override
    {
        copy(name, "VYBN");
        return kResultOk;
    }
    // The messages and attribute lists a component and its controller pass each other
    tresult PLUGIN_API createInstance(TUID cid, TUID iid, void **obj) override
    {
        *obj = nullptr;
        if (same(cid, IMessage_iid) && same(iid, IMessage_iid)) {
            *obj = static_cast<IMessage *>(new MessageObject);
            return kResultOk;
        }
        if (same(cid, IAttributeList_iid) && same(iid, IAttributeList_iid)) {
            *obj = static_cast<IAttributeList *>(new AttributeList);
            return kResultOk;
        }
        return kResultFalse;
    }
};

// Bytes a plugin reads its state from or writes it to
class Stream : public IBStream {
public:
    Stream() = default;
    explicit Stream(std::string data) : data(std::move(data)) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IBStream>(this, iid, IBStream_iid, obj); }
    VYBN_FOREVER // on the stack, for one call

    tresult PLUGIN_API read(void *buffer, int32 bytes, int32 *got) override
    {
        const size_t left = at < data.size() ? data.size() - at : 0;
        const size_t n = bytes > 0 ? std::min(left, static_cast<size_t>(bytes)) : 0;
        if (n > 0)
            std::memcpy(buffer, data.data() + at, n);
        at += n;
        if (got)
            *got = static_cast<int32>(n);
        return kResultOk;
    }
    tresult PLUGIN_API write(void *buffer, int32 bytes, int32 *put) override
    {
        if (bytes < 0 || at + static_cast<size_t>(bytes) > kMaxMessageBytes / 2)
            return kResultFalse;
        if (data.size() < at + static_cast<size_t>(bytes))
            data.resize(at + static_cast<size_t>(bytes));
        if (bytes > 0)
            std::memcpy(&data[at], buffer, static_cast<size_t>(bytes));
        at += static_cast<size_t>(bytes);
        if (put)
            *put = bytes;
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64 *result) override
    {
        int64 to = mode == kIBSeekSet ? pos : mode == kIBSeekCur ? static_cast<int64>(at) + pos : static_cast<int64>(data.size()) + pos;
        if (to < 0)
            return kResultFalse;
        at = static_cast<size_t>(to);
        if (result)
            *result = to;
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64 *pos) override
    {
        if (!pos)
            return kInvalidArgument;
        *pos = static_cast<int64>(at);
        return kResultOk;
    }

    std::string data;
    size_t at = 0;
};

// One parameter's changes in a block: points made ready before processing, never allocated after
class ParamQueue : public IParamValueQueue {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IParamValueQueue>(this, iid, IParamValueQueue_iid, obj); }
    VYBN_FOREVER

    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return count; }
    tresult PLUGIN_API getPoint(int32 index, int32 &offset, ParamValue &value) override
    {
        if (index < 0 || index >= count)
            return kInvalidArgument;
        offset = offsets[index];
        value = values[index];
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, ParamValue value, int32 &index) override
    {
        // In time order; a point at the last one's time replaces it, and a full queue keeps its newest
        if (count > 0 && offset <= offsets[count - 1]) {
            offset = offsets[count - 1];
            --count;
        } else if (count == kPoints) {
            --count;
        }
        offsets[count] = offset;
        values[count] = value;
        index = count++;
        return kResultOk;
    }

    static constexpr int32 kPoints = 64;
    ParamID id = 0;
    int32 count = 0;
    int32 offsets[kPoints] = {};
    ParamValue values[kPoints] = {};
};

class ParamChanges : public IParameterChanges {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IParameterChanges>(this, iid, IParameterChanges_iid, obj); }
    VYBN_FOREVER

    int32 PLUGIN_API getParameterCount() override { return count; }
    IParamValueQueue *PLUGIN_API getParameterData(int32 index) override { return index >= 0 && index < count ? &queues[static_cast<size_t>(index)] : nullptr; }
    IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &index) override
    {
        for (int32 i = 0; i < count; ++i) {
            if (queues[static_cast<size_t>(i)].id == id) {
                index = i;
                return &queues[static_cast<size_t>(i)];
            }
        }
        if (count >= static_cast<int32>(queues.size()))
            return nullptr;
        ParamQueue &q = queues[static_cast<size_t>(count)];
        q.id = id;
        q.count = 0;
        index = count++;
        return &q;
    }

    void add(ParamID id, int32 offset, ParamValue value)
    {
        int32 index = 0;
        if (IParamValueQueue *q = addParameterData(id, index))
            q->addPoint(offset, value, index);
    }

    std::vector<ParamQueue> queues = std::vector<ParamQueue>(512);
    int32 count = 0;
};

class EventList : public IEventList {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IEventList>(this, iid, IEventList_iid, obj); }
    VYBN_FOREVER

    int32 PLUGIN_API getEventCount() override { return count; }
    tresult PLUGIN_API getEvent(int32 index, Steinberg::Vst::Event &e) override
    {
        if (index < 0 || index >= count)
            return kInvalidArgument;
        e = events[static_cast<size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Steinberg::Vst::Event &e) override
    {
        if (count >= static_cast<int32>(events.size()))
            return kResultFalse;
        events[static_cast<size_t>(count++)] = e;
        return kResultOk;
    }

    std::vector<Steinberg::Vst::Event> events = std::vector<Steinberg::Vst::Event>(kMaxBlockEvents + 16 * 128 + 64);
    int32 count = 0;
};

class Vst3Format;
Vst3Format *current = nullptr; // the one the host made: the handler, the frame and the window reach it

class ComponentHandler : public IComponentHandler {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IComponentHandler>(this, iid, IComponentHandler_iid, obj); }
    VYBN_FOREVER

    tresult PLUGIN_API beginEdit(ParamID id) override;
    tresult PLUGIN_API performEdit(ParamID id, ParamValue value) override;
    tresult PLUGIN_API endEdit(ParamID id) override;
    tresult PLUGIN_API restartComponent(int32 flags) override;
};

class PlugFrame : public IPlugFrame {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void **obj) override { return answer<IPlugFrame>(this, iid, IPlugFrame_iid, obj); }
    VYBN_FOREVER

    tresult PLUGIN_API resizeView(IPlugView *view, ViewRect *size) override;
};

// What goes from the main thread to the audio thread: the controller's edits
enum class EditKind : uint32_t { Apply, ApplyAndReport, Begin, End };
struct Edit {
    EditKind kind = EditKind::Apply;
    ParamID id = 0;
    double value = 0.0; // normalised
};
// And back: values the processor got or made, for the controller
struct Update {
    ParamID id = 0;
    double value = 0.0;
};

constexpr ParamID kNoParam = 0xFFFFFFFFu;

struct Bus {
    std::vector<float *> channels;
    std::vector<std::vector<float>> scratch;
};

class Vst3Format : public Format {
public:
    Vst3Format() { current = this; }
    ~Vst3Format() override { current = nullptr; }

    bool open(const std::string &path, std::string &error) override
    {
        if (module_)
            return true;
        // A bundle (a folder) holds its module at Contents/x86_64-win/<its name>
        std::wstring file = widen(path);
        while (!file.empty() && (file.back() == L'\\' || file.back() == L'/'))
            file.pop_back();
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const size_t slash = file.find_last_of(L"\\/");
            file += L"\\Contents\\x86_64-win\\" + (slash == std::wstring::npos ? file : file.substr(slash + 1));
        }
        // Only the plugin's own folder and the system's are searched for its DLLs
        module_ = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module_) {
            error = "Couldn't load the plugin's file (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        using InitDll = bool (*)();
        using GetFactory = IPluginFactory *(PLUGIN_API *)();
        exitDll_ = reinterpret_cast<InitDll>(reinterpret_cast<void *>(GetProcAddress(module_, "ExitDll")));
        auto getFactory = reinterpret_cast<GetFactory>(reinterpret_cast<void *>(GetProcAddress(module_, "GetPluginFactory")));
        if (!getFactory) {
            error = "Not a VST3 plugin: it has no GetPluginFactory";
            return false;
        }
        if (auto init = reinterpret_cast<InitDll>(reinterpret_cast<void *>(GetProcAddress(module_, "InitDll")))) {
            if (!init()) {
                error = "The plugin refused to start (its InitDll failed)";
                return false;
            }
        }
        dllReady_ = true;
        factory_ = getFactory();
        if (!factory_) {
            error = "The plugin has no plugin factory";
            return false;
        }
        void *f = nullptr;
        if (factory_->queryInterface(IPluginFactory2_iid, &f) == kResultOk)
            factory2_ = static_cast<IPluginFactory2 *>(f);
        if (factory_->queryInterface(IPluginFactory3_iid, &f) == kResultOk) {
            factory3_ = static_cast<IPluginFactory3 *>(f);
            factory3_->setHostContext(&hostApp_);
        }
        return true;
    }

    bool scan(std::vector<Descriptor> &found, std::string &) override
    {
        PFactoryInfo factoryInfo{};
        factory_->getFactoryInfo(&factoryInfo);
        const std::string factoryVendor = ascii(factoryInfo.vendor, sizeof factoryInfo.vendor);
        const std::string url = ascii(factoryInfo.url, sizeof factoryInfo.url);
        const int32 count = factory_->countClasses();
        for (int32 i = 0; i < count && i < 1000; ++i) {
            Descriptor d;
            if (!describe(i, d))
                continue;
            if (d.vendor.empty())
                d.vendor = factoryVendor;
            d.url = url;
            found.push_back(d);
        }
        return true;
    }

    bool probe(const std::string &id, uint32_t &sidechain, std::string &error) override
    {
        IComponent *component = nullptr;
        if (!makeComponent(id, component, error))
            return false;
        sidechain = 0;
        const int32 buses = component->getBusCount(kAudio, kInput);
        for (int32 i = 0; i < buses && i < 32; ++i) {
            BusInfo bus{};
            if (component->getBusInfo(kAudio, kInput, i, bus) == kResultOk && bus.busType == kAux && (bus.channelCount == 1 || bus.channelCount == 2)) {
                sidechain = static_cast<uint32_t>(bus.channelCount);
                break;
            }
        }
        component->terminate();
        component->release();
        return true;
    }

    bool load(const std::string &id, LoadedInfo &info, std::string &error) override
    {
        if (!makeComponent(id, component_, error))
            return false;
        void *p = nullptr;
        if (component_->queryInterface(IAudioProcessor_iid, &p) != kResultOk || !p) {
            error = "The plugin has no audio processor";
            return false;
        }
        processor_ = static_cast<IAudioProcessor *>(p);
        if (processor_->canProcessSampleSize(kSample32) != kResultTrue) {
            error = "The plugin can't process 32-bit audio";
            return false;
        }
        // Its edit controller: the component itself, or a class of its own, connected to it
        if (component_->queryInterface(IEditController_iid, &p) == kResultOk && p) {
            controller_ = static_cast<IEditController *>(p);
        } else {
            TUID controllerId;
            if (component_->getControllerClassId(controllerId) == kResultOk &&
                factory_->createInstance(controllerId, IEditController_iid, &p) == kResultOk && p) {
                controller_ = static_cast<IEditController *>(p);
                if (controller_->initialize(&hostApp_) != kResultOk) {
                    controller_->release();
                    controller_ = nullptr;
                } else {
                    separate_ = true;
                }
            }
        }
        if (separate_) {
            if (component_->queryInterface(IConnectionPoint_iid, &p) == kResultOk && p)
                componentPoint_ = static_cast<IConnectionPoint *>(p);
            if (controller_->queryInterface(IConnectionPoint_iid, &p) == kResultOk && p)
                controllerPoint_ = static_cast<IConnectionPoint *>(p);
            if (componentPoint_ && controllerPoint_) {
                componentPoint_->connect(controllerPoint_);
                controllerPoint_->connect(componentPoint_);
            }
            Stream state;
            if (component_->getState(&state) == kResultOk) {
                state.at = 0;
                controller_->setComponentState(&state);
            }
        }
        if (controller_) {
            controller_->setComponentHandler(&handler_);
            if (controller_->queryInterface(IMidiMapping_iid, &p) == kResultOk && p)
                midiMapping_ = static_cast<IMidiMapping *>(p);
        }
        buildTables();

        info.name = name_.empty() ? id : name_;
        BusInfo bus{};
        if (component_->getBusCount(kAudio, kInput) > 0 && component_->getBusInfo(kAudio, kInput, 0, bus) == kResultOk)
            info.inputChannels = static_cast<uint32_t>(bus.channelCount);
        if (component_->getBusCount(kAudio, kOutput) > 0 && component_->getBusInfo(kAudio, kOutput, 0, bus) == kResultOk)
            info.outputChannels = static_cast<uint32_t>(bus.channelCount);
        info.notePorts = static_cast<uint32_t>(std::max<int32>(0, component_->getBusCount(kEvent, kInput)));
        info.params = controller_ && controller_->getParameterCount() > 0;
        info.state = true;
        // Its editor, made once to see that there is one that fits a window
        if (controller_) {
            if (IPlugView *view = controller_->createView(ViewType::kEditor)) {
                info.embedded = info.gui = view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue;
                view->release();
            }
        }
        return true;
    }

    bool loaded() const override { return component_ != nullptr; }

    bool activate(double rate, std::string &error) override
    {
        rate_ = rate;
        buildTables();
        buildBuses();
        ProcessSetup setup{kRealtime, kSample32, static_cast<int32>(kMaxFrames), rate};
        if (processor_->setupProcessing(setup) != kResultOk) {
            error = "The plugin refused the sample rate";
            return false;
        }
        if (component_->setActive(true) != kResultOk) {
            error = "The plugin refused to activate";
            return false;
        }
        shared->latency.store(latency());
        return true;
    }

    void deactivate() override { component_->setActive(false); }

    uint32_t latency() override { return processor_ ? processor_->getLatencySamples() : 0; }

    std::vector<ParamInfo> params() override
    {
        std::vector<ParamInfo> list;
        if (!controller_)
            return list;
        const int32 n = controller_->getParameterCount();
        for (int32 i = 0; i < n && i < 10000; ++i) {
            ParameterInfo info{};
            if (controller_->getParameterInfo(i, info) != kResultOk)
                continue;
            ParamInfo p;
            p.id = info.id;
            if (info.stepCount > 0 || (info.flags & ParameterInfo::kIsList))
                p.flags |= CLAP_PARAM_IS_STEPPED;
            if (info.flags & ParameterInfo::kIsList)
                p.flags |= CLAP_PARAM_IS_ENUM;
            if (info.flags & ParameterInfo::kIsWrapAround)
                p.flags |= CLAP_PARAM_IS_PERIODIC;
            if (info.flags & ParameterInfo::kIsHidden)
                p.flags |= CLAP_PARAM_IS_HIDDEN;
            if (info.flags & ParameterInfo::kIsReadOnly)
                p.flags |= CLAP_PARAM_IS_READONLY;
            if (info.flags & ParameterInfo::kIsBypass)
                p.flags |= CLAP_PARAM_IS_BYPASS;
            if (info.flags & ParameterInfo::kCanAutomate)
                p.flags |= CLAP_PARAM_IS_AUTOMATABLE;
            p.name = utf8(info.title, 128);
            const double steps = info.stepCount > 0 ? info.stepCount : 1.0;
            p.min = 0.0;
            p.max = steps;
            p.def = info.defaultNormalizedValue * steps;
            p.value = controller_->getParamNormalized(info.id) * steps;
            if (info.stepCount > 0)
                p.def = static_cast<double>(static_cast<int64_t>(p.def + 0.5)), p.value = static_cast<double>(static_cast<int64_t>(p.value + 0.5));
            list.push_back(p);
        }
        return list;
    }

    bool paramText(uint32_t id, double value, std::string &text) override
    {
        String128 buffer = {};
        if (!controller_ || controller_->getParamStringByValue(id, normalised(id, value), buffer) != kResultOk)
            return false;
        text = utf8(buffer, 128);
        return true;
    }

    bool hasState() const override { return true; }

    // The state: "VYB3", then the component's bytes and the controller's, each after its length
    bool saveState(std::string &out, std::string &error) override
    {
        Stream component, controller;
        if (component_->getState(&component) != kResultOk) {
            error = "The plugin couldn't save its state";
            return false;
        }
        if (separate_ && controller_->getState(&controller) != kResultOk)
            controller.data.clear();
        Writer w;
        w.raw("VYB3", 4);
        w.str(component.data);
        w.str(controller.data);
        out = std::move(w.data);
        return true;
    }

    bool loadState(const std::string &in, std::string &error) override
    {
        if (in.size() < 4 || in.compare(0, 4, "VYB3") != 0) {
            error = "The state isn't this plugin's";
            return false;
        }
        Reader body(in.data() + 4, in.size() - 4);
        Stream component(body.str()), controller(body.str());
        if (!body.ok()) {
            error = "The state is cut short";
            return false;
        }
        if (component_->setState(&component) != kResultOk) {
            error = "The plugin couldn't load the state";
            return false;
        }
        if (separate_) {
            component.at = 0;
            controller_->setComponentState(&component);
            if (!controller.data.empty())
                controller_->setState(&controller);
        }
        paramsRescan.fetch_or(CLAP_PARAM_RESCAN_VALUES | 0x80000000u);
        wake();
        return true;
    }

    bool guiEmbed(HWND frame, std::string &error) override
    {
        if (!makeView(error))
            return false;
        ViewRect size(0, 0, 640, 400);
        if (view_->getSize(&size) != kResultOk || size.getWidth() <= 0 || size.getHeight() <= 0)
            size = ViewRect(0, 0, 640, 400);
        resizeFrame(static_cast<uint32_t>(size.getWidth()), static_cast<uint32_t>(size.getHeight()));
        if (view_->attached(frame, kPlatformTypeHWND) != kResultOk) {
            error = "The plugin couldn't place its interface";
            dropView();
            return false;
        }
        return true;
    }

    void guiResize(uint32_t width, uint32_t height) override
    {
        if (!view_ || window_ || view_->canResize() != kResultTrue)
            return;
        ViewRect size(0, 0, static_cast<int32>(width), static_cast<int32>(height));
        view_->checkSizeConstraint(&size);
        if (view_->onSize(&size) == kResultOk)
            resizeFrame(static_cast<uint32_t>(size.getWidth()), static_cast<uint32_t>(size.getHeight()));
    }

    // A window of the host's own around the editor: VST3 has no floating interface of its own
    bool guiOpen(std::string &error) override
    {
        if (window_) {
            ShowWindow(window_, SW_SHOWNORMAL);
            SetForegroundWindow(window_);
            return true;
        }
        if (!makeView(error))
            return false;
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = windowProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // the arrow
            wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = L"VybnPluginWindow";
            registered = RegisterClassW(&wc) != 0;
        }
        ViewRect size(0, 0, 640, 400);
        if (view_->getSize(&size) != kResultOk || size.getWidth() <= 0 || size.getHeight() <= 0)
            size = ViewRect(0, 0, 640, 400);
        windowStyle_ = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (view_->canResize() == kResultTrue ? WS_THICKFRAME : 0);
        RECT rc{0, 0, size.getWidth(), size.getHeight()};
        AdjustWindowRect(&rc, windowStyle_, FALSE);
        window_ = CreateWindowExW(0, L"VybnPluginWindow", widen(name_).c_str(), windowStyle_, CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left,
                                  rc.bottom - rc.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) {
            error = "Couldn't make a window for the plugin";
            dropView();
            return false;
        }
        if (view_->attached(window_, kPlatformTypeHWND) != kResultOk) {
            error = "The plugin couldn't place its interface";
            guiClose();
            return false;
        }
        ShowWindow(window_, SW_SHOWNORMAL);
        return true;
    }

    void guiClose() override
    {
        dropView();
        if (window_) {
            HWND w = window_;
            window_ = nullptr;
            DestroyWindow(w);
        }
    }

    // The values the processor got or made, given to the controller; and, while inactive, the
    // app's changes, which no block will carry
    void housekeeping() override
    {
        if (!controller_)
            return;
        Update update;
        while (toController_.pop(update))
            controller_->setParamNormalized(update.id, update.value);
        if (!pluginActive()) {
            ParamChange change;
            while (shared->toPlugin.pop(change)) {
                if (change.kind != ChangeKind::Value)
                    continue;
                const double value = normalised(change.id, change.value);
                controller_->setParamNormalized(change.id, value);
                edits_.push({EditKind::Apply, change.id, value});
            }
        }
    }

    void unload() override
    {
        guiClose();
        if (controller_)
            controller_->setComponentHandler(nullptr);
        if (componentPoint_ && controllerPoint_) {
            componentPoint_->disconnect(controllerPoint_);
            controllerPoint_->disconnect(componentPoint_);
        }
        for (FUnknown *u : {static_cast<FUnknown *>(componentPoint_), static_cast<FUnknown *>(controllerPoint_), static_cast<FUnknown *>(midiMapping_)})
            if (u)
                u->release();
        componentPoint_ = controllerPoint_ = nullptr;
        midiMapping_ = nullptr;
        if (controller_) {
            if (separate_)
                controller_->terminate();
            controller_->release();
            controller_ = nullptr;
        }
        if (processor_)
            processor_->release();
        processor_ = nullptr;
        if (component_) {
            component_->terminate();
            component_->release();
            component_ = nullptr;
        }
        for (FUnknown *u : {static_cast<FUnknown *>(factory3_), static_cast<FUnknown *>(factory2_), static_cast<FUnknown *>(factory_)})
            if (u)
                u->release();
        factory_ = nullptr;
        factory2_ = nullptr;
        factory3_ = nullptr;
        if (dllReady_ && exitDll_)
            exitDll_();
        dllReady_ = false;
    }

    void process(uint32_t frames) override
    {
        if (mainOutput_ < 0) {
            silence(frames);
            shared->status = kProcessSleep;
            return;
        }
        if (!processing_) {
            processor_->setProcessing(true); // many plugins answer kNotImplemented: not a refusal
            processing_ = true;
        }
        inChanges_.count = 0;
        outChanges_.count = 0;
        events_.count = 0;

        // The app's changes, then the controller's edits, then the block's own at their frames
        ParamChange change;
        while (shared->toPlugin.pop(change)) {
            if (change.kind == ChangeKind::Value)
                applied(change.id, 0, normalised(change.id, change.value));
        }
        Edit edit;
        while (edits_.pop(edit)) {
            switch (edit.kind) {
            case EditKind::Apply:
            case EditKind::ApplyAndReport:
                inChanges_.add(edit.id, 0, edit.value);
                if (edit.kind == EditKind::ApplyAndReport)
                    shared->fromPlugin.push({ChangeKind::Value, edit.id, plain(edit.id, edit.value)});
                break;
            case EditKind::Begin:
            case EditKind::End:
                shared->fromPlugin.push({edit.kind == EditKind::Begin ? ChangeKind::GestureBegin : ChangeKind::GestureEnd, edit.id, 0.0});
                break;
            }
        }
        const uint32_t count = shared->eventCount <= kMaxBlockEvents ? shared->eventCount : kMaxBlockEvents;
        for (uint32_t i = 0; i < count; ++i) {
            const plugin::Event &e = shared->events[i];
            const int32 time = static_cast<int32>(e.frame < frames ? e.frame : (frames > 0 ? frames - 1 : 0));
            const int channel = e.channel >= 0 && e.channel < 16 ? e.channel : 0;
            switch (e.kind) {
            case EventKind::ParamValue:
                applied(e.id, time, normalised(e.id, e.value));
                break;
            case EventKind::NoteOn:
            case EventKind::NoteOff:
                note(time, channel, static_cast<int>(e.id & 127), e.kind == EventKind::NoteOn, e.value);
                break;
            case EventKind::Controller: {
                const uint32_t number = e.id == 128 ? static_cast<uint32_t>(kPitchBend) : e.id;
                if (number >= kCountCtrlNumber)
                    break;
                const ParamID id = midiMap_[static_cast<size_t>(channel)][number];
                if (id != kNoParam)
                    applied(id, time, e.value < 0.0 ? 0.0 : (e.value > 1.0 ? 1.0 : e.value));
                break;
            }
            case EventKind::AllNotesOff:
                for (int key = 0; key < 128; ++key)
                    if (held_[channel][key])
                        note(time, channel, key, false, 0.0);
                break;
            }
        }

        // The transport (positions in quarter notes, as the app counts beats)
        const TransportInfo &t = shared->transport;
        ProcessContext context{};
        context.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid |
                        ProcessContext::kBarPositionValid | (t.playing ? ProcessContext::kPlaying : 0);
        context.sampleRate = rate_;
        context.projectTimeSamples = static_cast<TSamples>(t.seconds * rate_ + 0.5);
        context.projectTimeMusic = t.beats;
        context.barPositionMusic = t.barStart;
        context.tempo = t.tempo > 1.0 && t.tempo < 1000.0 ? t.tempo : 120.0;
        context.timeSigNumerator = static_cast<int32>(t.beatsPerBar > 0 && t.beatsPerBar < 64 ? t.beatsPerBar : 4);
        context.timeSigDenominator = static_cast<int32>(t.beatUnit > 0 && t.beatUnit <= 32 ? t.beatUnit : 4);

        // Audio: the main buses read and write the shared block, the sidechain bus the key
        if (mainInput_ >= 0) {
            Bus &in = inputs_[static_cast<size_t>(mainInput_)];
            const uint32_t given = shared->inputChannels <= kMaxChannels ? shared->inputChannels : kMaxChannels;
            for (uint32_t c = 0; c < in.channels.size() && c < kMaxChannels; ++c)
                in.channels[c] = given > 0 ? shared->input[c < given ? c : given - 1] : in.scratch[c].data();
        }
        if (sidechainInput_ >= 0) {
            Bus &side = inputs_[static_cast<size_t>(sidechainInput_)];
            const uint32_t given = shared->keyChannels <= kMaxChannels ? shared->keyChannels : kMaxChannels;
            for (uint32_t c = 0; c < side.channels.size() && c < kMaxChannels; ++c)
                side.channels[c] = given > 0 ? shared->key[c < given ? c : given - 1] : side.scratch[c].data();
        }
        Bus &out = outputs_[static_cast<size_t>(mainOutput_)];
        for (uint32_t c = 0; c < out.channels.size() && c < kMaxChannels; ++c)
            out.channels[c] = shared->output[c];

        ProcessData data{};
        data.processMode = kRealtime;
        data.symbolicSampleSize = kSample32;
        data.numSamples = static_cast<int32>(frames);
        data.numInputs = static_cast<int32>(inBuffers_.size());
        data.numOutputs = static_cast<int32>(outBuffers_.size());
        data.inputs = inBuffers_.empty() ? nullptr : inBuffers_.data();
        data.outputs = outBuffers_.empty() ? nullptr : outBuffers_.data();
        data.inputParameterChanges = &inChanges_;
        data.outputParameterChanges = &outChanges_;
        data.inputEvents = &events_;
        data.outputEvents = &outEvents_;
        data.processContext = &context;
        outEvents_.count = 0;

        if (processor_->process(data) != kResultOk) {
            silence(frames);
            shared->status = kProcessError;
            return;
        }
        shared->status = kProcessContinue;
        // What the processor changed goes to the app and its controller
        for (int32 i = 0; i < outChanges_.count; ++i) {
            ParamQueue &q = outChanges_.queues[static_cast<size_t>(i)];
            if (q.count <= 0)
                continue;
            const double value = q.values[q.count - 1];
            shared->fromPlugin.push({ChangeKind::Value, q.id, plain(q.id, value)});
            toController_.push({q.id, value});
        }
        // A mono output plays on both sides
        if (out.channels.size() == 1)
            std::memcpy(shared->output[1], shared->output[0], frames * sizeof(float));
        shared->outputChannels = kMaxChannels;
    }

    void stopProcessing() override
    {
        if (processing_) {
            processor_->setProcessing(false);
            processing_ = false;
        }
    }

    // The controller's edits (main thread): to the processor and the app, through the audio
    // thread while it runs the plugin, else straight to the app
    void edited(EditKind kind, ParamID id, double value)
    {
        if (pluginActive()) {
            edits_.push({kind, id, value});
            return;
        }
        if (kind == EditKind::ApplyAndReport) {
            shared->fromPlugin.push({ChangeKind::Value, id, plain(id, value)});
            edits_.push({EditKind::Apply, id, value});
        } else if (kind == EditKind::Begin || kind == EditKind::End) {
            shared->fromPlugin.push({kind == EditKind::Begin ? ChangeKind::GestureBegin : ChangeKind::GestureEnd, id, 0.0});
        }
    }

    void restart(int32 flags)
    {
        if (flags & (kReloadComponent | kIoChanged | kLatencyChanged))
            restartRequested = true;
        if (flags & kLatencyChanged)
            latencyChanged = true;
        uint32_t rescan = 0;
        if (flags & kParamValuesChanged)
            rescan |= CLAP_PARAM_RESCAN_VALUES;
        if (flags & kParamTitlesChanged)
            rescan |= CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO;
        if (flags & kReloadComponent)
            rescan |= CLAP_PARAM_RESCAN_ALL;
        if (rescan)
            paramsRescan.fetch_or(rescan | 0x80000000u);
        wake();
    }

    void resized(IPlugView *view, ViewRect *size)
    {
        const int32 w = size->getWidth(), h = size->getHeight();
        if (window_) {
            RECT rc{0, 0, w, h};
            AdjustWindowRect(&rc, windowStyle_, FALSE);
            resizing_ = true;
            SetWindowPos(window_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            resizing_ = false;
        } else {
            resizeFrame(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
        view->onSize(size);
    }

private:
    bool describe(int32 index, Descriptor &d)
    {
        PClassInfo info{};
        if (factory_->getClassInfo(index, &info) != kResultOk || ascii(info.category, sizeof info.category) != kVstAudioEffectClass)
            return false;
        d.id = idOf(info.cid);
        d.name = ascii(info.name, sizeof info.name);
        PClassInfoW wide{};
        PClassInfo2 more{};
        if (factory3_ && factory3_->getClassInfoUnicode(index, &wide) == kResultOk) {
            d.name = utf8(wide.name, sizeof wide.name / sizeof(char16));
            d.vendor = utf8(wide.vendor, sizeof wide.vendor / sizeof(char16));
            d.version = utf8(wide.version, sizeof wide.version / sizeof(char16));
            d.features = featuresOf(ascii(wide.subCategories, sizeof wide.subCategories));
        } else if (factory2_ && factory2_->getClassInfo2(index, &more) == kResultOk) {
            d.vendor = ascii(more.vendor, sizeof more.vendor);
            d.version = ascii(more.version, sizeof more.version);
            d.features = featuresOf(ascii(more.subCategories, sizeof more.subCategories));
        } else {
            d.features = featuresOf("");
        }
        return !d.name.empty();
    }

    bool makeComponent(const std::string &id, IComponent *&component, std::string &error)
    {
        TUID cid;
        if (!cidOf(id, cid)) {
            error = "Not a VST3 plugin's id: \"" + id + "\"";
            return false;
        }
        for (int32 i = 0, n = factory_->countClasses(); i < n && name_.empty(); ++i) {
            Descriptor d;
            if (describe(i, d) && d.id == id)
                name_ = d.name;
        }
        void *p = nullptr;
        if (factory_->createInstance(cid, IComponent_iid, &p) != kResultOk || !p) {
            error = "The plugin file has no plugin \"" + id + "\"";
            return false;
        }
        component = static_cast<IComponent *>(p);
        if (component->initialize(&hostApp_) != kResultOk) {
            component->release();
            component = nullptr;
            error = "The plugin failed to start (initialize)";
            return false;
        }
        return true;
    }

    // The step counts (for plain values) and the MIDI mapping, read while the audio thread isn't using them
    void buildTables()
    {
        steps_.clear();
        if (controller_) {
            const int32 n = controller_->getParameterCount();
            for (int32 i = 0; i < n && i < 10000; ++i) {
                ParameterInfo info{};
                if (controller_->getParameterInfo(i, info) == kResultOk)
                    steps_.emplace_back(info.id, info.stepCount);
            }
        }
        std::sort(steps_.begin(), steps_.end());
        for (auto &channel : midiMap_)
            std::fill(std::begin(channel), std::end(channel), kNoParam);
        if (midiMapping_) {
            for (int16 channel = 0; channel < 16; ++channel) {
                for (int16 number = 0; number < kCountCtrlNumber; ++number) {
                    ParamID id = kNoParam;
                    if (midiMapping_->getMidiControllerAssignment(0, channel, number, id) == kResultTrue)
                        midiMap_[static_cast<size_t>(channel)][static_cast<size_t>(number)] = id;
                }
            }
        }
    }

    // Every audio bus active, the main ones stereo where the plugin agrees; the sidechain is the
    // first other input with one or two channels
    void buildBuses()
    {
        for (BusDirection dir : {kInput, kOutput}) {
            const int32 n = std::min<int32>(component_->getBusCount(kAudio, dir), 32);
            for (int32 i = 0; i < n; ++i)
                component_->activateBus(kAudio, dir, i, true);
        }
        if (component_->getBusCount(kEvent, kInput) > 0)
            component_->activateBus(kEvent, kInput, 0, true);
        std::vector<SpeakerArrangement> ins, outs;
        for (BusDirection dir : {kInput, kOutput}) {
            std::vector<SpeakerArrangement> &list = dir == kInput ? ins : outs;
            const int32 n = std::min<int32>(component_->getBusCount(kAudio, dir), 32);
            for (int32 i = 0; i < n; ++i) {
                SpeakerArrangement a = SpeakerArr::kStereo;
                processor_->getBusArrangement(dir, i, a);
                list.push_back(i == 0 ? SpeakerArr::kStereo : a);
            }
        }
        processor_->setBusArrangements(ins.empty() ? nullptr : ins.data(), static_cast<int32>(ins.size()), outs.empty() ? nullptr : outs.data(),
                                       static_cast<int32>(outs.size()));
        for (BusDirection dir : {kInput, kOutput}) {
            std::vector<Bus> &buses = dir == kInput ? inputs_ : outputs_;
            std::vector<AudioBusBuffers> &buffers = dir == kInput ? inBuffers_ : outBuffers_;
            int &main = dir == kInput ? mainInput_ : mainOutput_;
            buses.clear();
            buffers.clear();
            main = -1;
            const int32 n = std::min<int32>(component_->getBusCount(kAudio, dir), 32);
            for (int32 i = 0; i < n; ++i) {
                SpeakerArrangement a = 0;
                uint32_t channels = processor_->getBusArrangement(dir, i, a) == kResultOk ? channelsOf(a) : 0;
                BusInfo info{};
                const bool known = component_->getBusInfo(kAudio, dir, i, info) == kResultOk;
                if (channels == 0 && known)
                    channels = static_cast<uint32_t>(std::max<int32>(0, info.channelCount));
                channels = std::min<uint32_t>(channels, 64);
                Bus bus;
                bus.scratch.assign(channels, std::vector<float>(kMaxFrames, 0.0f));
                for (uint32_t c = 0; c < channels; ++c)
                    bus.channels.push_back(bus.scratch[c].data());
                if (main < 0 && known && info.busType == kMain)
                    main = static_cast<int>(buses.size());
                buses.push_back(std::move(bus));
            }
            if (main < 0 && !buses.empty())
                main = 0;
            for (Bus &bus : buses) {
                AudioBusBuffers b{};
                b.numChannels = static_cast<int32>(bus.channels.size());
                b.channelBuffers32 = bus.channels.empty() ? nullptr : bus.channels.data();
                buffers.push_back(b);
            }
            if (dir == kInput) {
                sidechainInput_ = -1;
                for (size_t i = 0; i < buses.size(); ++i) {
                    if (static_cast<int>(i) != main && (buses[i].channels.size() == 1 || buses[i].channels.size() == 2)) {
                        sidechainInput_ = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
    }

    int32 stepsOf(ParamID id) const
    {
        const auto found = std::lower_bound(steps_.begin(), steps_.end(), std::make_pair(id, std::numeric_limits<int32>::min()));
        return found != steps_.end() && found->first == id ? found->second : 0;
    }
    double normalised(ParamID id, double plainValue) const
    {
        const int32 steps = stepsOf(id);
        const double v = steps > 0 ? plainValue / steps : plainValue;
        return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    }
    double plain(ParamID id, double normalisedValue) const
    {
        const int32 steps = stepsOf(id);
        return steps > 0 ? static_cast<double>(static_cast<int64_t>(normalisedValue * steps + 0.5)) : normalisedValue;
    }

    // A change the processor gets in this block, and the controller after it (audio thread)
    void applied(ParamID id, int32 time, double value)
    {
        inChanges_.add(id, time, value);
        toController_.push({id, value});
    }

    void note(int32 time, int channel, int key, bool on, double velocity)
    {
        Steinberg::Vst::Event e{};
        e.busIndex = 0;
        e.sampleOffset = time;
        e.flags = Steinberg::Vst::Event::kIsLive;
        if (on) {
            e.type = Steinberg::Vst::Event::kNoteOnEvent;
            e.noteOn.channel = static_cast<int16>(channel);
            e.noteOn.pitch = static_cast<int16>(key);
            e.noteOn.velocity = static_cast<float>(velocity < 0.0 ? 0.0 : (velocity > 1.0 ? 1.0 : velocity));
            e.noteOn.noteId = -1;
        } else {
            e.type = Steinberg::Vst::Event::kNoteOffEvent;
            e.noteOff.channel = static_cast<int16>(channel);
            e.noteOff.pitch = static_cast<int16>(key);
            e.noteOff.velocity = 0.0f;
            e.noteOff.noteId = -1;
        }
        held_[channel][key] = on;
        events_.addEvent(e);
    }

    bool makeView(std::string &error)
    {
        if (view_) {
            error = "The plugin's interface is already open";
            return false;
        }
        view_ = controller_ ? controller_->createView(ViewType::kEditor) : nullptr;
        if (!view_) {
            error = "This plugin has no interface";
            return false;
        }
        if (view_->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
            error = "This plugin's interface doesn't open in a Windows window";
            dropView();
            return false;
        }
        view_->setFrame(&frame_);
        return true;
    }

    void dropView()
    {
        if (!view_)
            return;
        view_->removed();
        view_->setFrame(nullptr);
        view_->release();
        view_ = nullptr;
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        Vst3Format *self = current;
        if (message == WM_CLOSE) {
            guiClosed = true;
            wake();
            return 0;
        }
        if (message == WM_SIZE && self && self->view_ && self->window_ == hwnd && !self->resizing_ && wParam != SIZE_MINIMIZED) {
            ViewRect size(0, 0, LOWORD(lParam), HIWORD(lParam));
            self->view_->onSize(&size);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HostApplication hostApp_;
    ComponentHandler handler_;
    PlugFrame frame_;

    HMODULE module_ = nullptr;
    bool (*exitDll_)() = nullptr;
    bool dllReady_ = false;
    IPluginFactory *factory_ = nullptr;
    IPluginFactory2 *factory2_ = nullptr;
    IPluginFactory3 *factory3_ = nullptr;
    std::string name_;

    IComponent *component_ = nullptr;
    IAudioProcessor *processor_ = nullptr;
    IEditController *controller_ = nullptr;
    bool separate_ = false; // the controller is an object of its own
    IConnectionPoint *componentPoint_ = nullptr;
    IConnectionPoint *controllerPoint_ = nullptr;
    IMidiMapping *midiMapping_ = nullptr;
    IPlugView *view_ = nullptr;
    HWND window_ = nullptr; // the host's window around the editor, when it floats
    DWORD windowStyle_ = 0;
    bool resizing_ = false;

    std::vector<std::pair<ParamID, int32>> steps_; // by id
    ParamID midiMap_[16][kCountCtrlNumber];
    SharedRing<Edit, 1024> edits_;
    SharedRing<Update, 2048> toController_;

    // Audio thread
    double rate_ = 48000.0;
    bool processing_ = false;
    bool held_[16][128] = {};
    ParamChanges inChanges_;
    ParamChanges outChanges_;
    EventList events_;
    EventList outEvents_;
    std::vector<Bus> inputs_;
    std::vector<Bus> outputs_;
    std::vector<AudioBusBuffers> inBuffers_;
    std::vector<AudioBusBuffers> outBuffers_;
    int mainInput_ = -1;
    int sidechainInput_ = -1;
    int mainOutput_ = -1;
};

tresult PLUGIN_API ComponentHandler::beginEdit(ParamID id)
{
    if (current)
        current->edited(EditKind::Begin, id, 0.0);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue value)
{
    if (current)
        current->edited(EditKind::ApplyAndReport, id, value);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::endEdit(ParamID id)
{
    if (current)
        current->edited(EditKind::End, id, 0.0);
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags)
{
    if (current)
        current->restart(flags);
    return kResultOk;
}

tresult PLUGIN_API PlugFrame::resizeView(IPlugView *view, ViewRect *size)
{
    if (!view || !size || !current)
        return kInvalidArgument;
    current->resized(view, size);
    return kResultTrue;
}

}

std::unique_ptr<Format> makeVst3Format() { return std::unique_ptr<Format>(new Vst3Format); }

}
