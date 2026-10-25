#include "daw/views/DevicesPanel.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "daw/widgets/Format.h"
#include "audio/Effects.h"
#include "audio/Modulation.h"

namespace daw {

using namespace ui;

namespace {

// What a modulator's knobs read their parameters from (it isn't a device in a chain)
class ModulatorDescription : public audio::Device {
public:
    explicit ModulatorDescription(audio::ModulatorKind kind) : kind_(kind) {}
    audio::DeviceKind kind() const override { return audio::DeviceKind::Modulator; }
    bool matches(const audio::DeviceSetting &setting) const override { return setting.kind == audio::DeviceKind::Modulator; }
    void applySetting(const audio::DeviceSetting &) override {}
    std::vector<audio::DeviceParam> params() const override { return audio::modulatorParams(kind_); }

private:
    audio::ModulatorKind kind_;
};

// A modulator's value as a bar in a groove: from the middle when it swings both ways
class ModulatorMeter : public Component {
public:
    ModulatorMeter(std::function<float()> value, std::function<bool()> bipolar) : value_(std::move(value)), bipolar_(std::move(bipolar)) {}
    Vec2 measure(const Theme &) const override { return {200.0f, 10.0f}; }
    void arrange(const Rect &rect, const Theme &) override { rect_ = rect; }
    void draw(DrawList &list, const Theme &theme) const override
    {
        list.add(theme.surface(rect_, Shape::Pill, Elevation::Inset, 0.5f));
        const float v = value_ ? value_() : 0.0f;
        const Rect inner = rect_.inset(2.0f);
        if (bipolar_ && bipolar_()) {
            const float mid = inner.x + inner.w * 0.5f;
            const float at = mid + std::clamp(v, -1.0f, 1.0f) * inner.w * 0.5f;
            list.add(theme.accent({std::min(mid, at), inner.y, std::max(2.0f, std::fabs(at - mid)), inner.h}, Shape::Pill, 0.85f));
        } else {
            list.add(theme.accent({inner.x, inner.y, std::max(2.0f, inner.w * std::clamp(v, 0.0f, 1.0f)), inner.h}, Shape::Pill, 0.85f));
        }
    }

private:
    std::function<float()> value_;
    std::function<bool()> bipolar_;
};

}

DevicesView::DevicesView(Context &context, ProjectStore &store, DeviceCatalogue &catalogue, DevicesHooks hooks)
    : Container(Direction::Column), context_(context), store_(store), catalogue_(catalogue), hooks_(std::move(hooks))
{
    setAlign(Align::Stretch);
    auto &scroll = add<ScrollView>();
    scroll.setGrow(1.0f);
    auto &rows = scroll.content().add<Container>(Direction::Column);
    rows.setAlign(Align::Start);
    cards_ = &rows.add<Container>(Direction::Row);
    cards_->setAlign(Align::Start);
    modulators_ = &rows.add<Container>(Direction::Row);
    modulators_->setAlign(Align::Start);
}

std::string DevicesView::targetName() const
{
    if (master_)
        return "Master";
    const Track *t = project().track(project().selectedTrack);
    if (!t)
        return "No track selected";
    const std::string latency = hooks_.latencyText ? hooks_.latencyText(project().selectedTrack) : std::string();
    return latency.empty() ? t->name : t->name + "  /  " + latency;
}

DeviceAccess DevicesView::access(const DevicePlace &place)
{
    DeviceAccess a;
    a.setting = [this, place] { return project().device(place); };
    a.edit = [this, place] { return store_.editDevice(place); };
    a.device = [this, place]() -> audio::Device * { return hooks_.device ? hooks_.device(place) : nullptr; };
    a.playhead = hooks_.playhead;
    a.sidechain = [this, place] { return sidechainOffer(project(), place); };
    a.revision = [this] { return store_.revision(); };
    if (hooks_.write)
        a.write = [this, place](const audio::DeviceParam &param, double value) { return hooks_.write(place, param, value); };
    return a;
}

std::vector<audio::DeviceSetting> &DevicesView::editChain(bool master, int track)
{
    return master ? store_.editMasterEffects() : store_.editTrack(track).effects;
}

std::string DevicesView::signature()
{
    std::string s = master_ ? "M" : "T" + std::to_string(project().selectedTrack);
    auto describe = [&](const DevicePlace &place, const audio::DeviceSetting &d) {
        s += "," + d.key() + (takesSidechain(d) ? "~" : "");
        // A keyed effect says who keys it in its header: rebuilt when the key or the source's name changes
        if (d.sidechain != kNoId) {
            const Track *source = project().findTrack(d.sidechain);
            s += "@" + std::to_string(d.sidechain) + (source ? source->name : std::string());
        }
        audio::Device *device = hooks_.device ? hooks_.device(place) : nullptr;
        s += device ? ":" + std::to_string(device->params().size()) : ":-";
        if (plugin::Slot *slot = device ? device->slot() : nullptr)
            s += ":" + std::to_string(static_cast<int>(slot->state())) + ":" + std::to_string(slot->paramsVersion());
    };
    const Track *t = master_ ? nullptr : project().track(project().selectedTrack);
    if (t && t->kind == TrackKind::Instrument) {
        s += "|M";
        for (const audio::DeviceSetting &d : t->midiEffects)
            s += "," + d.key();
        s += "|I";
        describe(DevicePlace::instrumentOf(project().selectedTrack), t->instrument);
    }
    const std::vector<audio::DeviceSetting> *chain = project().effects(master_, project().selectedTrack);
    if (!chain)
        return s + "-";
    s += "|E";
    for (size_t i = 0; i < chain->size(); ++i)
        describe(DevicePlace::effect(master_, project().selectedTrack, static_cast<int>(i)), (*chain)[i]);
    // The modulators and where their routes go (not their depths: the sliders follow those)
    if (t) {
        s += "|O";
        for (const audio::Modulator &m : t->modulators)
            s += "," + std::to_string(m.id) + m.setting.id;
        auto routes = [&s](const std::vector<audio::ModRoute> &list) {
            for (const audio::ModRoute &r : list)
                s += "/" + std::to_string(r.source) + ":" + std::to_string(r.param);
        };
        routes(t->instrument.modulation);
        for (const audio::DeviceSetting &d : t->effects)
            routes(d.modulation);
        routes(t->mixModulation);
    }
    return s;
}

void DevicesView::handleInput(const Input &input)
{
    if (!rebuilding_ && signature() != built_) {
        rebuilding_ = true;
        context_.post([this] {
            built_ = signature();
            rebuild();
            rebuilding_ = false;
        });
    }
    Container::handleInput(input);
}

std::string DevicesView::sourceText(const audio::DeviceSetting &setting, bool instrument)
{
    std::string text = deviceKindText(setting, instrument);
    if (setting.kind == audio::DeviceKind::Plugin) {
        const CatalogueEntry *e = catalogue_.find(DeviceCatalogue::keyOf(setting));
        text += e ? "  /  " + e->source : "  /  not installed";
    }
    return text;
}

void DevicesView::pickEffect(Rect anchor)
{
    if (hooks_.pick)
        hooks_.pick(false, master_, project().selectedTrack, anchor);
}

void DevicesView::rebuild()
{
    cards_->clear();
    buildModulators();
    const std::vector<audio::DeviceSetting> *chain = project().effects(master_, project().selectedTrack);
    if (!chain) {
        cards_->add<Label>("Select a track to see its instrument and effects, or switch to Master", TextStyle::Body, TextTone::Muted);
        return;
    }
    const Track *t = master_ ? nullptr : project().track(project().selectedTrack);
    midiDescriptions_.clear();
    if (t && t->kind == TrackKind::Instrument) {
        // Its MIDI effects, then the button that adds one
        for (size_t i = 0; i < t->midiEffects.size(); ++i)
            buildMidiCard(*cards_, i);
        if (t->midiEffects.size() < audio::kMaxMidiEffects) {
            Button &add = cards_->add<Button>();
            Button *raw = &add;
            add.setLabel("MIDI effect").setIcon(Icon::Plus).setVariant(ButtonVariant::Flat).setSize(Size::Small).setAlignSelf(Align::Start);
            add.onClick([this, raw] { addMidiEffect(raw->rect()); });
        }
        if (t->hasInstrument())
            buildCard(*cards_, DevicePlace::instrumentOf(project().selectedTrack));
        else
            buildEmptyInstrument(*cards_);
    }
    for (size_t i = 0; i < chain->size(); ++i)
        buildCard(*cards_, DevicePlace::effect(master_, project().selectedTrack, static_cast<int>(i)));

    // The place for the next effect
    if (chain->size() < audio::kMaxEffects) {
        Button &add = cards_->add<Button>();
        Button *raw = &add;
        add.setLabel(chain->empty() ? "Add an effect" : "Add effect").setIcon(Icon::Plus).setSize(Size::Small).setAlignSelf(Align::Start);
        add.onClick([this, raw] { pickEffect(raw->rect()); });
    }
}

void DevicesView::buildModulators()
{
    modulators_->clear();
    modulatorDescriptions_.clear();
    const Track *t = master_ ? nullptr : project().track(project().selectedTrack);
    if (!t)
        return;
    for (size_t i = 0; i < t->modulators.size(); ++i)
        buildModulatorCard(*modulators_, i);
    if (t->modulators.size() < audio::kMaxModulators) {
        Button &add = modulators_->add<Button>();
        Button *raw = &add;
        add.setLabel("Modulator").setIcon(Icon::Plus).setVariant(ButtonVariant::Flat).setSize(Size::Small).setAlignSelf(Align::Start);
        add.onClick([this, raw] { addModulator(raw->rect()); });
    }
}

void DevicesView::addModulator(Rect anchor)
{
    const int track = project().selectedTrack;
    std::vector<MenuItem> items;
    for (int i = 0; i < audio::kModulatorKindCount; ++i) {
        const auto kind = static_cast<audio::ModulatorKind>(i);
        items.push_back(MenuItem::item(audio::modulatorInfo(kind).name, [this, track, kind] {
            const Track *t = project().track(track);
            if (!t || t->modulators.size() >= audio::kMaxModulators)
                return;
            // The next number in the track
            uint32_t id = 1;
            for (const audio::Modulator &m : t->modulators)
                id = std::max(id, m.id + 1);
            audio::Modulator m{id, audio::modulatorSetting(kind)};
            m.setting.name = std::string(audio::modulatorInfo(kind).name) + " " + std::to_string(id);
            store_.editTrack(track).modulators.push_back(std::move(m));
        }));
    }
    openContextMenu(Context::activeOr(context_), items, {anchor.x, anchor.y + anchor.h + 6.0f});
}

std::string DevicesView::targetName(const DevicePlace &place, uint32_t param)
{
    if (param == audio::kVolumeParam)
        return "Track volume";
    if (param == audio::kPanParam)
        return "Track pan";
    const audio::DeviceSetting *setting = project().device(place);
    audio::Device *device = hooks_.device ? hooks_.device(place) : nullptr;
    std::string name = "Parameter " + std::to_string(param);
    if (device) {
        for (const audio::DeviceParam &p : device->params())
            if (p.id == param)
                name = p.name;
    }
    return setting ? name + "  (" + setting->name + ")" : name;
}

void DevicesView::addTarget(uint32_t source, Rect anchor)
{
    const int track = project().selectedTrack;
    const Track *t = project().track(track);
    if (!t)
        return;
    // A route added once: to a device's parameter, or the track's volume or pan
    auto route = [this, track, source](const DevicePlace &place, uint32_t param) {
        const Track *tr = project().track(track);
        if (!tr)
            return;
        auto add = [source, param](std::vector<audio::ModRoute> &routes) {
            for (const audio::ModRoute &r : routes)
                if (r.source == source && r.param == param)
                    return;
            routes.push_back({source, param, 0.5f});
        };
        if (param == audio::kVolumeParam || param == audio::kPanParam)
            add(store_.editTrack(track).mixModulation);
        else if (audio::DeviceSetting *d = store_.editDevice(place))
            add(d->modulation);
    };
    std::vector<MenuItem> items;
    auto deviceMenu = [&](const DevicePlace &place) {
        const audio::DeviceSetting *setting = project().device(place);
        audio::Device *device = hooks_.device ? hooks_.device(place) : nullptr;
        if (!setting || !device || !device->automatable())
            return;
        std::vector<MenuItem> params;
        for (const audio::DeviceParam &p : shownParams(device)) {
            if (params.size() >= 96)
                break;
            const uint32_t id = p.id;
            params.push_back(MenuItem::item(p.name, [route, place, id] { route(place, id); }));
        }
        if (!params.empty())
            items.push_back(MenuItem::sub(setting->name, params));
    };
    if (t->kind == TrackKind::Instrument && t->hasInstrument())
        deviceMenu(DevicePlace::instrumentOf(track));
    for (size_t i = 0; i < t->effects.size(); ++i)
        deviceMenu(DevicePlace::effect(false, track, static_cast<int>(i)));
    items.push_back(MenuItem::divider());
    items.push_back(MenuItem::item("Track volume", [route, track] { route(DevicePlace::instrumentOf(track), audio::kVolumeParam); }));
    items.push_back(MenuItem::item("Track pan", [route, track] { route(DevicePlace::instrumentOf(track), audio::kPanParam); }));
    openContextMenu(Context::activeOr(context_), items, {anchor.x, anchor.y + anchor.h + 6.0f});
}

void DevicesView::buildModulatorCard(Container &row, size_t index)
{
    const int track = project().selectedTrack;
    const Track *t = project().track(track);
    if (!t || index >= t->modulators.size())
        return;
    const audio::Modulator &m = t->modulators[index];
    audio::ModulatorKind kind = audio::ModulatorKind::Lfo;
    audio::modulatorFromKey(m.setting.id, kind);
    modulatorDescriptions_.push_back(std::make_unique<ModulatorDescription>(kind));
    audio::Device *description = modulatorDescriptions_.back().get();
    const uint32_t id = m.id;
    // Its setting, found again by its number each time
    auto setting = [this, track, id]() -> const audio::DeviceSetting * {
        const Track *tr = project().track(track);
        if (!tr)
            return nullptr;
        for (const audio::Modulator &mod : tr->modulators)
            if (mod.id == id)
                return &mod.setting;
        return nullptr;
    };
    DeviceAccess a;
    a.setting = setting;
    a.edit = [this, track, id, setting]() -> audio::DeviceSetting * {
        if (!setting())
            return nullptr;
        for (audio::Modulator &mod : store_.editTrack(track).modulators)
            if (mod.id == id)
                return &mod.setting;
        return nullptr;
    };
    a.device = [description]() -> audio::Device * { return description; };

    auto &card = row.add<Panel>(Direction::Column);
    card.setAlign(Align::Stretch);
    card.setAlignSelf(Align::Start);
    card.setDepth(Depth::Normal);
    auto &head = card.add<Container>(Direction::Row);
    head.setAlign(Align::Center);
    auto &titles = head.add<Container>(Direction::Column);
    titles.setAlign(Align::Start);
    titles.add<Label>(m.setting.name, TextStyle::Subtitle).setMaxWidth(200.0f);
    titles.add<Label>("Modulator", TextStyle::Caption, TextTone::Accent);
    head.add<Spacer>();
    head.add<SmallToggle>("Bypass", [setting] { return setting() && setting()->bypass; }, [a](bool on) {
        if (audio::DeviceSetting *s = a.edit())
            s->bypass = on;
    });
    head.add<Button>([this, track, id] {
            const Track *tr = project().track(track);
            if (!tr)
                return;
            // It goes, and every route from it
            Track &edited = store_.editTrack(track);
            edited.modulators.erase(std::remove_if(edited.modulators.begin(), edited.modulators.end(),
                                                   [id](const audio::Modulator &mod) { return mod.id == id; }),
                                    edited.modulators.end());
            auto drop = [id](std::vector<audio::ModRoute> &routes) {
                routes.erase(std::remove_if(routes.begin(), routes.end(), [id](const audio::ModRoute &r) { return r.source == id; }), routes.end());
            };
            drop(edited.instrument.modulation);
            for (audio::DeviceSetting &d : edited.effects)
                drop(d.modulation);
            drop(edited.mixModulation);
        })
        .setIcon(Icon::Close)
        .setVariant(ButtonVariant::Flat)
        .setShape(Shape::Circle)
        .setSize(Size::Small);
    // Its value now
    const size_t slot = index;
    card.add<ModulatorMeter>([this, track, slot] { return hooks_.modulatorValue ? hooks_.modulatorValue(track, slot) : 0.0f; },
                             [setting] { return setting() && audio::modulatorBipolar(*setting()); });
    // Its knobs (a Steps modulator's steps as a row of small ones)
    const std::vector<audio::DeviceParam> params = description->params();
    if (kind == audio::ModulatorKind::Steps) {
        auto &top = card.add<Grid>(3);
        for (size_t i = 0; i < 3; ++i)
            addParamControl(context_, top, a, params[i], 44.0f);
        auto &steps = card.add<Grid>(8);
        for (size_t i = 3; i < params.size(); ++i)
            addParamControl(context_, steps, a, params[i], 30.0f, 7, true, false);
    } else {
        auto &grid = card.add<Grid>(static_cast<int>(std::min<size_t>(params.size(), 3)));
        for (const audio::DeviceParam &p : params)
            addParamControl(context_, grid, a, p, 44.0f);
    }

    // Its targets: each with its depth (-100% to +100%) and a remove button
    card.add<Label>("Targets", TextStyle::Caption, TextTone::Secondary);
    struct Target {
        DevicePlace place;
        bool mix;
        uint32_t param;
    };
    std::vector<Target> targets;
    auto collect = [&](const DevicePlace &place, const std::vector<audio::ModRoute> &routes, bool mix) {
        for (const audio::ModRoute &r : routes)
            if (r.source == id)
                targets.push_back({place, mix, r.param});
    };
    if (t->kind == TrackKind::Instrument)
        collect(DevicePlace::instrumentOf(track), t->instrument.modulation, false);
    for (size_t i = 0; i < t->effects.size(); ++i)
        collect(DevicePlace::effect(false, track, static_cast<int>(i)), t->effects[i].modulation, false);
    collect(DevicePlace::instrumentOf(track), t->mixModulation, true);
    for (const Target &target : targets) {
        // The route, looked up each time
        auto routeOf = [this, track, id, target](bool edit) -> audio::ModRoute * {
            const Track *tr = project().track(track);
            if (!tr)
                return nullptr;
            std::vector<audio::ModRoute> *routes = nullptr;
            if (target.mix) {
                routes = edit ? &store_.editTrack(track).mixModulation : const_cast<std::vector<audio::ModRoute> *>(&tr->mixModulation);
            } else {
                const audio::DeviceSetting *d = project().device(target.place);
                if (!d)
                    return nullptr;
                routes = edit ? &store_.editDevice(target.place)->modulation : const_cast<std::vector<audio::ModRoute> *>(&d->modulation);
            }
            for (audio::ModRoute &r : *routes)
                if (r.source == id && r.param == target.param)
                    return &r;
            return nullptr;
        };
        auto &line = card.add<Container>(Direction::Row);
        line.setAlign(Align::Center);
        line.add<Label>(targetName(target.place, target.param), TextStyle::Caption, TextTone::Primary).setMaxWidth(150.0f);
        line.add<Spacer>();
        const audio::ModRoute *now = routeOf(false);
        Slider &depth = line.add<Slider>(now ? (now->depth + 1.0f) * 0.5f : 0.75f, [routeOf](float v) {
            if (routeOf(false))
                routeOf(true)->depth = std::round((v * 2.0f - 1.0f) * 100.0f) / 100.0f;
        });
        depth.setSize(Size::Small).setMinSize({90.0f, 0.0f});
        Slider *raw = &depth;
        line.add<FrameHook>([routeOf, raw](const Input &) {
            // Following the project (undo), unless it is being dragged to the same value
            if (const audio::ModRoute *r = routeOf(false)) {
                const float v = (r->depth + 1.0f) * 0.5f;
                if (std::fabs(raw->value() - v) > 0.006f)
                    raw->setValue(v);
            }
        });
        line.add<LiveLabel>([routeOf] {
            const audio::ModRoute *r = routeOf(false);
            return r ? format("%+.0f%%", r->depth * 100.0f) : std::string();
        }, TextStyle::Caption, TextTone::Muted).setMinSize({36.0f, 0.0f});
        line.add<Button>([this, track, id, target] {
                auto drop = [id, target](std::vector<audio::ModRoute> &routes) {
                    routes.erase(std::remove_if(routes.begin(), routes.end(),
                                                [&](const audio::ModRoute &r) { return r.source == id && r.param == target.param; }),
                                 routes.end());
                };
                if (target.mix)
                    drop(store_.editTrack(track).mixModulation);
                else if (audio::DeviceSetting *d = store_.editDevice(target.place))
                    drop(d->modulation);
            })
            .setIcon(Icon::Close)
            .setVariant(ButtonVariant::Flat)
            .setShape(Shape::Circle)
            .setSize(Size::Small);
    }
    Button &add = card.add<Button>();
    Button *raw = &add;
    add.setLabel("Add target").setIcon(Icon::Plus).setVariant(ButtonVariant::Flat).setSize(Size::Small).setAlignSelf(Align::Start);
    add.onClick([this, raw, id] { addTarget(id, raw->rect()); });
}

void DevicesView::addMidiEffect(Rect anchor)
{
    const int track = project().selectedTrack;
    std::vector<MenuItem> items;
    for (int i = 0; i < audio::kMidiEffectTypeCount; ++i) {
        const auto type = static_cast<audio::MidiEffectType>(i);
        items.push_back(MenuItem::item(audio::midiEffectInfo(type).name, [this, track, type] {
            const Track *t = project().track(track);
            if (t && t->kind == TrackKind::Instrument && t->midiEffects.size() < audio::kMaxMidiEffects)
                store_.editTrack(track).midiEffects.push_back(audio::midiEffectSetting(type));
        }));
    }
    openContextMenu(Context::activeOr(context_), items, {anchor.x, anchor.y + anchor.h + 6.0f});
}

void DevicesView::buildMidiCard(Container &row, size_t index)
{
    const int track = project().selectedTrack;
    const Track *t = project().track(track);
    if (!t || index >= t->midiEffects.size())
        return;
    midiDescriptions_.push_back(audio::makeMidiEffect(t->midiEffects[index]));
    audio::MidiEffect *description = midiDescriptions_.back().get();
    // The setting, looked up each time (the track's list may change under it)
    auto setting = [this, track, index]() -> const audio::DeviceSetting * {
        const Track *tr = project().track(track);
        return tr && index < tr->midiEffects.size() ? &tr->midiEffects[index] : nullptr;
    };
    DeviceAccess a;
    a.setting = setting;
    a.edit = [this, track, index, setting]() -> audio::DeviceSetting * {
        return setting() ? &store_.editTrack(track).midiEffects[index] : nullptr;
    };
    a.device = [description]() -> audio::Device * { return description; };

    auto &card = row.add<Panel>(Direction::Column);
    card.setAlign(Align::Stretch);
    card.setAlignSelf(Align::Start);
    card.setDepth(Depth::Normal);
    auto &head = card.add<Container>(Direction::Row);
    head.setAlign(Align::Center);
    auto &titles = head.add<Container>(Direction::Column);
    titles.setAlign(Align::Start);
    titles.add<Label>(t->midiEffects[index].name, TextStyle::Subtitle).setMaxWidth(200.0f);
    titles.add<Label>("MIDI effect", TextStyle::Caption, TextTone::Muted);
    head.add<Spacer>();
    head.add<SmallToggle>("Bypass", [setting] { return setting() && setting()->bypass; }, [a](bool on) {
        if (audio::DeviceSetting *s = a.edit())
            s->bypass = on;
    });
    auto icon = [&head](Icon i, std::function<void()> action) {
        head.add<Button>(std::move(action)).setIcon(i).setVariant(ButtonVariant::Flat).setShape(Shape::Circle).setSize(Size::Small);
    };
    auto move = [this, track, index](int by) {
        const Track *tr = project().track(track);
        const size_t to = index + static_cast<size_t>(by);
        if (tr && index < tr->midiEffects.size() && to < tr->midiEffects.size())
            std::swap(store_.editTrack(track).midiEffects[index], store_.editTrack(track).midiEffects[to]);
    };
    icon(Icon::Previous, [move] { move(-1); });
    icon(Icon::Next, [move] { move(1); });
    icon(Icon::Close, [this, track, index] {
        const Track *tr = project().track(track);
        if (tr && index < tr->midiEffects.size()) {
            std::vector<audio::DeviceSetting> &list = store_.editTrack(track).midiEffects;
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
        }
    });
    const std::vector<audio::DeviceParam> params = description->params();
    auto &grid = card.add<Grid>(static_cast<int>(std::min<size_t>(params.size(), 4)));
    for (const audio::DeviceParam &param : params)
        addParamControl(context_, grid, a, param, 48.0f);
}

void DevicesView::buildEmptyInstrument(Container &row)
{
    auto &card = row.add<Panel>(Direction::Column);
    card.setAlign(Align::Stretch);
    card.setAlignSelf(Align::Start);
    card.setDepth(Depth::Normal);
    card.setElevation(Elevation::Inset);
    card.add<Label>("No instrument", TextStyle::Subtitle);
    card.add<Label>("The track's notes are silent until it has one.", TextStyle::Caption, TextTone::Muted).setWrapWidth(240.0f);
    Button &choose = card.add<Button>();
    Button *raw = &choose;
    const int track = project().selectedTrack;
    choose.setLabel("Choose an instrument").setIcon(Icon::Keys).setVariant(ButtonVariant::Accent).setShape(Shape::Pill).setSize(Size::Small);
    choose.setAlignSelf(Align::Start);
    choose.onClick([this, raw, track] {
        if (hooks_.pick)
            hooks_.pick(true, false, track, raw->rect());
    });
    card.add<Label>("or drag one from the browser (Tab) onto the track", TextStyle::Caption, TextTone::Muted);
}

void DevicesView::buildCard(Container &row, const DevicePlace &place)
{
    const audio::DeviceSetting *setting = project().device(place);
    if (!setting)
        return;
    const bool instrument = place.instrument();
    audio::Device *device = hooks_.device ? hooks_.device(place) : nullptr;

    auto &card = row.add<Panel>(Direction::Column);
    card.setAlign(Align::Stretch);
    card.setAlignSelf(Align::Start);
    card.setDepth(Depth::Normal);

    // Its name and kind, and its buttons
    auto &head = card.add<Container>(Direction::Row);
    head.setAlign(Align::Center);
    auto &titles = head.add<Container>(Direction::Column);
    titles.setAlign(Align::Start);
    const std::string title = setting->name.empty() ? setting->id : setting->name;
    if (instrument) {
        // The instrument's name opens the picker, with the instrument in use at its top
        Button &name = titles.add<Button>();
        Button *raw = &name;
        name.setLabel(title).setVariant(ButtonVariant::Flat).setSize(Size::Medium).setAlignSelf(Align::Start);
        name.onClick([this, raw, place] {
            if (hooks_.pick)
                hooks_.pick(true, false, place.track, raw->rect());
        });
    } else {
        titles.add<Label>(title, TextStyle::Subtitle).setMaxWidth(230.0f);
    }
    titles.add<Label>(sourceText(*setting, instrument), TextStyle::Caption, instrument ? TextTone::Accent : TextTone::Muted)
        .setMaxWidth(230.0f);
    // A keyed effect listens elsewhere: say so at a glance
    if (!instrument && setting->sidechain != kNoId && takesSidechain(*setting)) {
        const Track *source = project().findTrack(setting->sidechain);
        if (source)
            titles.add<Label>("Keyed by " + source->name, TextStyle::Caption, TextTone::Accent).setMaxWidth(230.0f);
    }
    head.add<Spacer>();
    auto icon = [&head](Icon i, std::function<void()> action) -> Button & {
        return head.add<Button>(std::move(action)).setIcon(i).setVariant(ButtonVariant::Flat).setShape(Shape::Circle).setSize(Size::Small);
    };
    if (device && device->hasMeter()) {
        head.add<LiveLabel>(
            [this, place] {
                audio::Device *d = hooks_.device ? hooks_.device(place) : nullptr;
                const float db = d ? d->meter() : 0.0f;
                return db > 0.05f ? format("-%.1f dB", db) : std::string("0.0 dB");
            },
            TextStyle::Caption, TextTone::Accent);
    }
    if (!instrument) {
        head.add<SmallToggle>("Bypass", [this, place] { return project().device(place) && project().device(place)->bypass; },
                              [this, place](bool on) {
                                  if (audio::DeviceSetting *s = store_.editDevice(place))
                                      s->bypass = on;
                              });
    }
    icon(Icon::Sliders, [this, place] {
        if (hooks_.openDevice)
            hooks_.openDevice(place);
    });
    if (instrument) {
        Button &change = head.add<Button>();
        Button *raw = &change;
        change.setLabel("Change").setVariant(ButtonVariant::Flat).setSize(Size::Small);
        change.onClick([this, raw, place] {
            if (hooks_.pick)
                hooks_.pick(true, false, place.track, raw->rect());
        });
        icon(Icon::Close, [this, place] {
            if (project().track(place.track))
                store_.editTrack(place.track).instrument = {};
        });
    } else {
        auto move = [this, place](int by) {
            const std::vector<audio::DeviceSetting> *c = project().effects(place.master, place.track);
            const size_t from = static_cast<size_t>(place.index);
            const size_t to = static_cast<size_t>(place.index + by);
            if (c && from < c->size() && to < c->size()) {
                std::vector<audio::DeviceSetting> &edited = editChain(place.master, place.track);
                std::swap(edited[from], edited[to]);
            }
        };
        icon(Icon::Previous, [move] { move(-1); });
        icon(Icon::Next, [move] { move(1); });
        icon(Icon::Close, [this, place] {
            const std::vector<audio::DeviceSetting> *c = project().effects(place.master, place.track);
            if (c && place.index >= 0 && static_cast<size_t>(place.index) < c->size()) {
                std::vector<audio::DeviceSetting> &edited = editChain(place.master, place.track);
                edited.erase(edited.begin() + place.index);
            }
        });
    }
    buildBody(card, place);
}

void DevicesView::buildBody(Container &card, const DevicePlace &place)
{
    audio::Device *device = hooks_.device ? hooks_.device(place) : nullptr;
    const DeviceAccess a = access(place);
    const audio::DeviceSetting *setting = project().device(place);

    // A plugin that isn't playing: why, and Reload
    const std::string status = pluginStatus(device);
    if (!status.empty()) {
        card.add<LiveLabel>([this, place] { return pluginStatus(hooks_.device ? hooks_.device(place) : nullptr); }, TextStyle::Caption,
                            TextTone::Muted)
            .setWrapWidth(300.0f);
        plugin::Slot *slot = device ? device->slot() : nullptr;
        if (slot && slot->state() != plugin::Slot::State::Loading) {
            card.add<Button>([this, place] {
                    audio::Device *d = hooks_.device ? hooks_.device(place) : nullptr;
                    if (plugin::Slot *s = d ? d->slot() : nullptr)
                        s->reload();
                })
                .setLabel("Reload")
                .setSize(Size::Small)
                .setAlignSelf(Align::Start);
        }
        return;
    }

    // Its first few parameters
    const std::vector<audio::DeviceParam> params = shownParams(device);
    const size_t count = std::min(params.size(), kCardParams);
    if (count > 0) {
        auto &grid = card.add<Grid>(static_cast<int>(count));
        for (size_t i = 0; i < count; ++i)
            addParamControl(context_, grid, a, params[i], 56.0f);
    } else if (device) {
        card.add<Label>("No parameters to show", TextStyle::Caption, TextTone::Muted);
    }

    // A compressor or gate: the track whose sound it listens to
    if (setting && takesSidechain(*setting))
        addSidechainControl(context_, card, a);

    // A library instrument's state: loading, its memory, or why it is silent
    if (setting && setting->kind == audio::DeviceKind::Sampler && hooks_.instrumentStatus) {
        const int track = place.track;
        card.add<LiveLabel>([this, track] { return hooks_.instrumentStatus(track); }, TextStyle::Caption, TextTone::Muted)
            .setWrapWidth(260.0f);
    }

    // The rest of it, and how a plugin runs
    plugin::Slot *slot = device ? device->slot() : nullptr;
    if (params.size() > count || slot) {
        auto &foot = card.add<Container>(Direction::Row);
        foot.setAlign(Align::Center);
        std::string more = "Open";
        if (params.size() > count)
            more = format("All %.0f parameters", static_cast<double>(params.size()));
        if (slot && slot->canEmbedEditor())
            more += params.size() > count ? std::string(", interface") : std::string(" its interface");
        foot.add<Button>([this, place] {
                if (hooks_.openDevice)
                    hooks_.openDevice(place);
            })
            .setLabel(more)
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
        foot.add<Spacer>();
        if (slot) {
            foot.add<LiveLabel>(
                [this, place] {
                    audio::Device *d = hooks_.device ? hooks_.device(place) : nullptr;
                    plugin::Slot *s = d ? d->slot() : nullptr;
                    if (!s)
                        return std::string();
                    std::string note = s->contained() ? "Sandboxed" : "Not sandboxed";
                    if (s->latency() > 0)
                        note += format(",  %.0f samples latency", static_cast<double>(s->latency()));
                    if (s->missed() > 0)
                        note += format(",  %.0f late", static_cast<double>(s->missed()));
                    return note;
                },
                TextStyle::Caption, TextTone::Muted);
        }
    }
}

}
