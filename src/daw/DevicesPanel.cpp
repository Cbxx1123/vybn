#include "DevicesPanel.h"

#include <algorithm>
#include <utility>

#include "Format.h"
#include "audio/Effects.h"

namespace daw {

using namespace ui;

DevicesView::DevicesView(Context &context, ProjectStore &store, DeviceCatalogue &catalogue, DevicesHooks hooks)
    : Container(Direction::Column), context_(context), store_(store), catalogue_(catalogue), hooks_(std::move(hooks))
{
    setAlign(Align::Stretch);
    auto &scroll = add<ScrollView>();
    scroll.setGrow(1.0f);
    cards_ = &scroll.content().add<Container>(Direction::Row);
    cards_->setAlign(Align::Start);
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
        s += "," + d.key();
        audio::Device *device = hooks_.device ? hooks_.device(place) : nullptr;
        s += device ? ":" + std::to_string(device->params().size()) : ":-";
        if (plugin::Slot *slot = device ? device->slot() : nullptr)
            s += ":" + std::to_string(static_cast<int>(slot->state())) + ":" + std::to_string(slot->paramsVersion());
    };
    const Track *t = master_ ? nullptr : project().track(project().selectedTrack);
    if (t && t->kind == TrackKind::Instrument) {
        s += "|I";
        describe(DevicePlace::instrumentOf(project().selectedTrack), t->instrument);
    }
    const std::vector<audio::DeviceSetting> *chain = project().effects(master_, project().selectedTrack);
    if (!chain)
        return s + "-";
    s += "|E";
    for (size_t i = 0; i < chain->size(); ++i)
        describe(DevicePlace::effect(master_, project().selectedTrack, static_cast<int>(i)), (*chain)[i]);
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
    const std::vector<audio::DeviceSetting> *chain = project().effects(master_, project().selectedTrack);
    if (!chain) {
        cards_->add<Label>("Select a track to see its instrument and effects, or switch to Master", TextStyle::Body, TextTone::Muted);
        return;
    }
    const Track *t = master_ ? nullptr : project().track(project().selectedTrack);
    if (t && t->kind == TrackKind::Instrument) {
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
    titles.add<Label>(setting->name.empty() ? setting->id : setting->name, TextStyle::Subtitle).setMaxWidth(230.0f);
    titles.add<Label>(sourceText(*setting, instrument), TextStyle::Caption, instrument ? TextTone::Accent : TextTone::Muted)
        .setMaxWidth(230.0f);
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
