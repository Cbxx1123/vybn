#include "DeviceView.h"

#include <algorithm>
#include <cctype>

#include "Format.h"
#include "SynthView.h"
#include "Widgets.h"

namespace daw {

using namespace ui;

namespace {

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr int kColumns = 6;
constexpr const char *kNoGroup = ""; // the group of parameters without a module

// A module as shown: "/A Filters/" as "A Filters", "Filter/Envelope" as "Filter / Envelope"
std::string groupName(const std::string &module)
{
    std::string out;
    size_t start = 0;
    while (start <= module.size()) {
        size_t end = module.find('/', start);
        if (end == std::string::npos)
            end = module.size();
        if (end > start)
            out += (out.empty() ? "" : " / ") + module.substr(start, end - start);
        start = end + 1;
    }
    return out.empty() ? std::string("Other") : out;
}

}

DeviceView::DeviceView(Context &context, DeviceAccess access, std::string title, std::string caption, bool effect, bool synth)
    : Container(Direction::Column), context_(context), access_(std::move(access)), title_(std::move(title)), caption_(std::move(caption)),
      effect_(effect), synth_(synth)
{
    setAlign(Align::Stretch);
    head_ = &add<Container>(Direction::Row);
    head_->setAlign(Align::Center);

    pages_ = &add<Pages>();
    pages_->setGrow(1.0f);

    // A synth's own panel first (it opens on it)
    if (synth_)
        pages_->add<SynthView>(context_, access_);

    // Controls: the groups on the left, the search and the controls on the right
    auto &controls = pages_->add<Container>(Direction::Row);
    controls.setAlign(Align::Stretch);
    groupScroll_ = &controls.add<ScrollView>();
    groupScroll_->setMinSize({180.0f, 0.0f});
    groups_ = &groupScroll_->content().add<ListView>();
    auto &right = controls.add<Container>(Direction::Column);
    right.setAlign(Align::Stretch);
    right.setGrow(1.0f);
    search_ = &right.add<TextField>();
    search_->setPlaceholder("Search parameters");
    search_->onChange([this](const std::string &) { request(controlsBuilt_, "?", &DeviceView::rebuildControls); });
    auto &scroll = right.add<ScrollView>();
    scroll.setGrow(1.0f);
    grid_ = &scroll.content().add<Container>(Direction::Column);
    grid_->setAlign(Align::Stretch);

    // Interface: the plugin's own, made when it can show one
    interfacePage_ = &pages_->add<Container>(Direction::Column);
    interfacePage_->setAlign(Align::Stretch);
}

plugin::Slot *DeviceView::slot() const
{
    audio::Device *d = access_.device ? access_.device() : nullptr;
    return d ? d->slot() : nullptr;
}

void DeviceView::showInterface(bool interface)
{
    chosePage_ = true;
    wantInterface_ = interface && !editor_;
    pages_->setCurrent(interface && editor_ ? interfaceIndex() : controlsIndex());
}

std::string DeviceView::headSignature() const
{
    audio::Device *d = access_.device ? access_.device() : nullptr;
    const plugin::Slot *s = d ? d->slot() : nullptr;
    return std::string(d ? "D" : "-") + (s ? std::to_string(static_cast<int>(s->state())) + (s->canEmbedEditor() ? "E" : "") +
                                                 (s->hasEditor() ? "W" : "")
                                           : "");
}

std::string DeviceView::controlsSignature() const
{
    audio::Device *d = access_.device ? access_.device() : nullptr;
    const plugin::Slot *s = d ? d->slot() : nullptr;
    return std::string(d ? "D" : "-") + (s ? std::to_string(s->paramsVersion()) + ":" + std::to_string(s->params().size()) : "") + "|" +
           group_ + "|" + search_->text();
}

void DeviceView::request(std::string &built, std::string now, void (DeviceView::*rebuild)())
{
    bool &posted = &built == &headBuilt_ ? headPosted_ : controlsPosted_;
    if (posted)
        return;
    if (now != "?" && now == built)
        return;
    posted = true;
    context_.post([this, &built, &posted, rebuild] {
        posted = false;
        (this->*rebuild)();
        built = &built == &headBuilt_ ? headSignature() : controlsSignature();
    });
}

void DeviceView::handleInput(const Input &input)
{
    request(headBuilt_, headSignature(), &DeviceView::rebuildHead);
    request(controlsBuilt_, controlsSignature(), &DeviceView::rebuildControls);
    Container::handleInput(input);
}

void DeviceView::rebuildHead()
{
    head_->clear();
    // Its name is on its tab or its window's title: here what it is and where
    head_->add<Label>(caption_, TextStyle::Caption, TextTone::Muted);
    head_->add<Spacer>();
    plugin::Slot *s = slot();

    // A synth: its panel, or the device's parameters as a list
    if (synth_) {
        auto &choice = head_->add<SegmentedControl>(std::vector<std::string>{"Synth", "All parameters"}, pages_->current() == 0 ? 0 : 1,
                                                    [this](int i) { pages_->setCurrent(i == 0 ? 0 : controlsIndex()); });
        choice.setSize(Size::Small);
        SegmentedControl *raw = &choice;
        head_->add<FrameHook>([this, raw](const Input &) { raw->setSelected(pages_->current() == 0 ? 0 : 1); });
    }

    // Controls | Interface, for a plugin that can show its own inside the app
    if (s && s->state() == plugin::Slot::State::Ready && s->canEmbedEditor()) {
        if (!editor_) {
            auto finder = [access = access_]() -> plugin::Slot * {
                audio::Device *d = access.device ? access.device() : nullptr;
                return d ? d->slot() : nullptr;
            };
            auto &view = interfacePage_->add<PluginEditorView>(finder, title_);
            view.setGrow(1.0f);
            editor_ = &view;
            if (wantInterface_) {
                wantInterface_ = false;
                pages_->setCurrent(interfaceIndex());
            }
        }
        auto &pages = head_->add<SegmentedControl>(std::vector<std::string>{"Controls", "Interface"}, static_cast<int>(pages_->current()),
                                                   [this](int i) { showInterface(i == 1); });
        pages.setSize(Size::Small);
        SegmentedControl *raw = &pages;
        head_->add<FrameHook>([this, raw](const Input &) { raw->setSelected(static_cast<int>(pages_->current())); });
        // A plugin with nothing for the controls to show opens on its interface
        if (!chosePage_) {
            chosePage_ = true;
            if (shownParams(access_.device ? access_.device() : nullptr).empty())
                pages_->setCurrent(interfaceIndex());
        }
    } else if (s && s->state() == plugin::Slot::State::Ready && s->hasEditor()) {
        // Only as a window of its own
        head_->add<Button>([this] {
                if (plugin::Slot *p = slot())
                    p->toggleEditor();
            })
            .setLabel("Its own window")
            .setIcon(Icon::PopOut)
            .setVariant(ButtonVariant::Flat)
            .setSize(Size::Small);
    }
    if (effect_) {
        head_->add<SmallToggle>("Bypass", [this] { return access_.setting && access_.setting() && access_.setting()->bypass; },
                                [this](bool on) {
                                    if (audio::DeviceSetting *st = access_.edit ? access_.edit() : nullptr)
                                        st->bypass = on;
                                });
    }
    if (s && s->state() != plugin::Slot::State::Ready && s->state() != plugin::Slot::State::Loading) {
        head_->add<Button>([this] {
                if (plugin::Slot *p = slot())
                    p->reload();
            })
            .setLabel("Reload")
            .setSize(Size::Small);
    }
}

void DeviceView::rebuildControls()
{
    audio::Device *d = access_.device ? access_.device() : nullptr;
    grid_->clear();
    groups_->clear();
    const std::string status = pluginStatus(d);
    if (!status.empty()) {
        grid_->add<LiveLabel>([this] { return pluginStatus(access_.device ? access_.device() : nullptr); }, TextStyle::Body, TextTone::Muted)
            .setWrapWidth(420.0f);
        return;
    }
    const std::vector<audio::DeviceParam> params = shownParams(d);

    // The groups, with how many each has (a device without groups has just All)
    std::vector<std::pair<std::string, size_t>> groups;
    for (const audio::DeviceParam &p : params) {
        const std::string key = p.module.empty() ? std::string(kNoGroup) : p.module;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto &g) { return g.first == key; });
        if (it == groups.end())
            groups.emplace_back(key, 1);
        else
            ++it->second;
    }
    auto addGroup = [this](const std::string &name, const std::string &label, size_t count) {
        auto &item = groups_->add<ListItem>(label, [this, name] { group_ = name; });
        item.setTrailing(std::to_string(count)).setSize(Size::Small).setSelected(group_ == name);
    };
    addGroup("", "All", params.size());
    if (groups.size() > 1 || (groups.size() == 1 && groups.front().first != kNoGroup))
        for (const auto &g : groups)
            addGroup(g.first, g.first == kNoGroup ? std::string("Other") : groupName(g.first), g.second);

    // The controls the group and the search let through, in rows of knobs
    const std::string search = lower(search_->text());
    std::vector<const audio::DeviceParam *> shown;
    size_t matching = 0;
    for (const audio::DeviceParam &p : params) {
        if (!group_.empty() && (p.module.empty() ? std::string(kNoGroup) : p.module) != group_)
            continue;
        if (!search.empty() && lower(p.name + " " + p.module).find(search) == std::string::npos)
            continue;
        ++matching;
        if (shown.size() < kPageParams)
            shown.push_back(&p);
    }
    if (params.empty()) {
        grid_->add<Label>(d ? "No parameters to show" : "Starting...", TextStyle::Body, TextTone::Muted);
        return;
    }
    if (shown.empty()) {
        grid_->add<Label>("Nothing matches \"" + search_->text() + "\"", TextStyle::Body, TextTone::Muted);
        return;
    }
    // Under a group heading each time the group changes (when showing all)
    Grid *row = nullptr;
    std::string module = "\x01";
    for (const audio::DeviceParam *p : shown) {
        if (p->module != module || !row) {
            module = p->module;
            if (group_.empty() && !module.empty())
                grid_->add<Label>(groupName(module), TextStyle::Caption, TextTone::Accent);
            row = &grid_->add<Grid>(kColumns);
        }
        addParamControl(context_, *row, access_, *p, 52.0f, 16);
    }
    if (matching > shown.size())
        grid_->add<Label>("Showing " + std::to_string(shown.size()) + " of " + std::to_string(matching) +
                              ": pick a group or search to see the rest",
                          TextStyle::Caption, TextTone::Muted);
}

}
