#include "daw/views/Mixer.h"

#include <algorithm>
#include <string>
#include <utility>

#include "daw/model/AutomationLanes.h"
#include "daw/audio/AutomationWriter.h"
#include "daw/widgets/Format.h"
#include "daw/model/Routing.h"

namespace daw {

using namespace ui;

namespace {

constexpr float kStripWidth = 112.0f;
constexpr size_t kVisibleSends = 3; // send bars shown before the rest scroll
constexpr float kSendBarHeight = 18.0f;
constexpr float kSendRows = kVisibleSends * kSendBarHeight + 12.0f; // the height of those
constexpr float kTrackMaxGain = 2.0f;   // +6 dB
constexpr float kSendMaxGain = 1.0f;    // a send's: 0 dB at most
constexpr float kMasterMaxGain = 2.0f;  // the project's master volume
constexpr float kMonitorMaxGain = 1.5f; // the AudioSystem's master gain range

// A scroll view that asks for little width, so in a narrow window the strips scroll across
// instead of pushing the master strip out of view
class StripScroller : public ScrollView {
public:
    Vec2 measure(const Theme &theme) const override { return {200.0f, ScrollView::measure(theme).y}; }
};

Container &centredRow(Container &parent)
{
    auto &row = parent.add<Container>(Direction::Row);
    row.setAlign(Align::Center).setJustify(Justify::Center);
    return row;
}

// One send of a strip's track: a slim bar for its level into one return (on a fader's taper, 0 dB at
// the right end), with the return's letter before it, in the accent colour while it sends, and a dot
// after it for a pre-fader send. Dragging along it (or clicking) sets the level, making the send when
// there is none; double-click puts it at 0 dB; right-click for pre or post fader, or to remove it.
// While the song plays and the track's writing is armed, dragging writes its lane instead.
class SendBar : public Component {
public:
    SendBar(Context &context, ProjectStore &store, int track, ObjectId to, std::string letter, AutomationWriter *writer)
        : context_(context), store_(store), track_(track), to_(to), letter_(std::move(letter)), writer_(writer)
    {
    }

    // Narrower in a scrolling list, which takes room for its scrollbar from the strip's width
    SendBar &setCompact(bool compact)
    {
        width_ = compact ? kStripWidth - 66.0f : kStripWidth - 24.0f;
        return *this;
    }

    Vec2 measure(const Theme &) const override { return {width_, kSendBarHeight}; }

    void handleInput(const Input &input) override
    {
        const Track *t = store_.project().track(track_);
        const Send *send = sendNow();
        const double level = !t ? 0.0 : writer_ ? trackValueAt(*t, sendParam(to_), writer_->playhead()) : send ? send->level : 0.0;
        position_ = faderPosition(static_cast<float>(level), kSendMaxGain);
        sending_ = send && level > 0.0;
        preFader_ = send && send->preFader;
        hovered_ = rect_.contains(input.mouse);
        if (input.secondaryPressed && hovered_) {
            input.secondaryPressed = false;
            openMenu(input.mouse);
            return;
        }
        if (input.pressed && hovered_) {
            const bool twice = input.time - lastPress_ < 0.3;
            lastPress_ = input.time;
            if (twice) {
                set(1.0f);
                return;
            }
            dragging_ = true;
        }
        if (dragging_ && input.down) {
            const Rect bar = barRect();
            set(faderGain(std::clamp((input.mouse.x - bar.x) / std::max(1.0f, bar.w), 0.0f, 1.0f), kSendMaxGain));
        }
        if (!input.down)
            dragging_ = false;
    }

    void draw(DrawList &list, const Theme &theme) const override
    {
        Fingerprint key;
        key.add(position_);
        key.add(sending_);
        key.add(preFader_);
        key.add(hovered_ || dragging_);
        key.add(letter_);
        cache_.draw(list, theme, rect_, key.value(), [&] {
            const Color ink = theme.textColor(sending_ ? TextTone::Accent : TextTone::Secondary);
            list.add(theme.text(letter_, {rect_.x, rect_.y, kLetter, rect_.h}, theme.fontSize(TextStyle::Caption), TextAlign::Start, ink));
            const Rect bar = barRect();
            const float cy = bar.y + bar.h * 0.5f;
            list.add(theme.surface({bar.x, cy - 3.0f, bar.w, 6.0f}, Shape::Pill, Elevation::Inset, 0.6f));
            const float x = bar.x + bar.w * position_;
            if (position_ > 0.0f)
                list.add(theme.accent({bar.x, cy - 2.0f, std::max(4.0f, x - bar.x), 4.0f}, Shape::Pill, sending_ ? 1.0f : 0.4f));
            list.add(theme.surface({x - 6.0f, cy - 6.0f, 12.0f, 12.0f}, Shape::Circle, Elevation::Raised,
                                   hovered_ || dragging_ ? 1.0f : 0.7f, Finish::Convex));
            if (preFader_) // a pre-fader send: a dot after the bar
                list.add(theme.accent({rect_.x + rect_.w - 5.0f, cy - 2.5f, 5.0f, 5.0f}, Shape::Circle, 1.0f));
        });
    }

private:
    static constexpr float kLetter = 16.0f; // the letter's column

    Rect barRect() const { return {rect_.x + kLetter + 6.0f, rect_.y, std::max(1.0f, rect_.w - kLetter - 18.0f), rect_.h}; }

    const Send *sendNow() const
    {
        const Track *t = store_.project().track(track_);
        return t ? findSend(*t, to_) : nullptr;
    }

    void set(float level)
    {
        const Project &project = store_.project();
        if (!project.track(track_))
            return;
        // A new send only where it may go (not into a return that feeds this track)
        if (!sendNow()) {
            if (!canSendTo(project, track_, project.trackIndex(to_)))
                return;
            store_.editMix(track_).sends.push_back({to_, writer_ && writer_->armed(track_) ? 0.0f : level, false});
        }
        if (writer_ && writer_->touchTrack(track_, sendParam(to_), level))
            return;
        const Send *send = sendNow();
        if (send && send->level != level)
            findSend(store_.editMix(track_), to_)->level = level;
    }

    void openMenu(Vec2 at)
    {
        std::vector<MenuItem> items;
        ProjectStore &store = store_;
        const int track = track_;
        const ObjectId to = to_;
        items.push_back(MenuItem::toggle("Pre-fader", [&store, track, to] {
            const Track *t = store.project().track(track);
            const Send *send = t ? findSend(*t, to) : nullptr;
            return send && send->preFader;
        }, [&store, track, to] {
            const Track *t = store.project().track(track);
            if (t && findSend(*t, to)) {
                Send *send = findSend(store.editMix(track), to);
                send->preFader = !send->preFader;
            }
        }));
        items.push_back(MenuItem::item("Remove send", [&store, track, to] {
            const Track *t = store.project().track(track);
            if (!t || !findSend(*t, to))
                return;
            // The send and its lane, in one edit
            removeSend(store.editTrack(track), to);
        }).when([this] { return sendNow() != nullptr; }));
        openContextMenu(context_, std::move(items), at);
    }

    Context &context_;
    ProjectStore &store_;
    int track_;
    ObjectId to_;
    std::string letter_;
    AutomationWriter *writer_;
    float width_ = kStripWidth - 24.0f;
    float position_ = 0.0f;
    bool sending_ = false;
    bool preFader_ = false;
    bool hovered_ = false;
    bool dragging_ = false;
    double lastPress_ = -1.0;
    DrawCache cache_{"SendBar"}; // draw, while nothing it reads has changed
};

}

// ChannelStrip

ChannelStrip::ChannelStrip(Context &context, ProjectStore &store, int index, const std::vector<float> &levels,
                           AutomationWriter *writer, FoldState *folds)
    : Panel(Direction::Column), context_(context), store_(store), index_(index), levels_(levels), writer_(writer)
{
    // What plays at the playhead: an automated volume or pan follows its lane
    auto playing = [&store, index, writer](uint32_t param) {
        const Track *t = store.project().track(index);
        if (!t)
            return 0.0;
        return writer ? trackValueAt(*t, param, writer->playhead()) : (param == audio::kPanParam ? t->pan : t->gain);
    };

    setAlign(Align::Stretch);
    setDepth(Depth::Normal);
    setMinSize({kStripWidth, 0.0f});

    auto trackName = [&store, index] {
        const Track *t = store.project().track(index);
        return t ? t->name : std::string();
    };
    const Track *own = store.project().track(index);
    if (folds && own && own->kind == TrackKind::Group) {
        // A group: the chevron that folds it, beside its name
        auto &row = centredRow(*this);
        row.setGap(4.0f);
        const ObjectId id = own->id;
        row.add<FoldChevron>([folds, id] { return folds->folded(id); }, [folds, id](bool on) { folds->set(id, on); });
        name_ = &row.add<LiveLabel>(trackName, TextStyle::Caption);
    } else {
        name_ = &add<LiveLabel>(trackName, TextStyle::Caption);
    }
    name_->setAlignment(TextAlign::Center);
    // Where it goes: click for the choices
    output_ = &add<LiveLabel>([&store, index] { return outputLabel(store.project(), index); }, TextStyle::Caption,
                              TextTone::Secondary);
    output_->setAlignment(TextAlign::Center);

    pan_ = &add<Knob>(0.5f, [&store, index, writer](float v) {
        const float pan = v * 2.0f - 1.0f;
        if (store.project().track(index) && !(writer && writer->touchTrack(index, audio::kPanParam, pan)))
            store.editMix(index).pan = pan;
    });
    pan_->setDiameter(46.0f).setAlignSelf(Align::Center);

    auto &faders = centredRow(*this);
    faders.setAlign(Align::Stretch).setGrow(1.0f);
    faders.add<Fader>(
              [playing] { return faderPosition(static_cast<float>(playing(audio::kVolumeParam)), kTrackMaxGain); },
              [&store, index, writer](float position) {
                  const float gain = faderGain(position, kTrackMaxGain);
                  if (store.project().track(index) && !(writer && writer->touchTrack(index, audio::kVolumeParam, gain)))
                      store.editMix(index).gain = gain;
              })
        .setMaxGain(kTrackMaxGain)
        .setDefault(faderPosition(1.0f, kTrackMaxGain));
    meter_ = &faders.add<LevelMeter>(Direction::Column);
    meter_->setThickness(8.0f).setLength(60.0f);

    add<LiveLabel>([&store, index, playing] {
        if (!store.project().track(index))
            return std::string();
        return decibels(static_cast<float>(playing(audio::kVolumeParam))) + "  " +
               panText(static_cast<float>(playing(audio::kPanParam)));
    }, TextStyle::Caption, TextTone::Secondary).setAlignment(TextAlign::Center);

    // A send bar for each return but itself, then one for each group it sends to (only the sends that exist,
    // labelled "G1"), packed one under another (the strips are made again when the tracks or those sends
    // change). Up to kVisibleSends show at once; more sit in a small scrolling list, so a track with many sends
    // never pushes its M/S/R row out of the strip.
    const Project &project = store.project();
    const Track *self = project.track(index);
    struct Bar {
        ObjectId to;
        std::string label;
    };
    std::vector<Bar> bars;
    for (size_t r = 0; r < project.tracks.size(); ++r) {
        const Track &ret = project.tracks[r];
        if (ret.kind == TrackKind::Return && static_cast<int>(r) != index && self)
            bars.push_back({ret.id, returnLetterOf(project, static_cast<int>(r))});
    }
    for (int g : groupSends(project, index))
        bars.push_back({project.tracks[static_cast<size_t>(g)].id, groupSendLabel(project, g)});
    if (!bars.empty()) {
        Container *list;
        if (bars.size() > kVisibleSends) {
            auto &scroll = add<ScrollView>();
            scroll.setContent(std::make_unique<ListView>());
            scroll.setViewHeight(kSendRows).setFillWidth();
            list = &scroll.content();
        } else {
            list = &add<ListView>();
            list->setAlignSelf(Align::Center);
        }
        for (const Bar &bar : bars)
            list->add<SendBar>(context, store, index, bar.to, bar.label, writer).setCompact(bars.size() > kVisibleSends);
    }

    auto &flags = centredRow(*this);
    auto toggle = [&store, index, &flags](const char *label, bool Track::*member) {
        flags.add<SmallToggle>(
            label,
            [&store, index, member] {
                const Track *t = store.project().track(index);
                return t && t->*member;
            },
            [&store, index, member](bool on) {
                if (store.project().track(index))
                    store.editMix(index).*member = on;
            });
    };
    toggle("M", &Track::mute);
    toggle("S", &Track::solo);
    if (self && !self->bus()) // buses record nothing
        toggle("R", &Track::armed);
}

void ChannelStrip::openOutputMenu()
{
    const Track *t = project().track(index_);
    if (!t)
        return;
    ProjectStore &store = store_;
    const int index = index_;
    auto choice = [&store, index](const std::string &label, ObjectId id) {
        return MenuItem::toggle(label, [&store, index, id] {
            const Track *tr = store.project().track(index);
            return tr && tr->output == id;
        }, [&store, index, id] {
            const Track *tr = store.project().track(index);
            if (tr && tr->output != id)
                store.editMix(index).output = id;
        });
    };
    std::vector<MenuItem> items{choice("Master", kNoId)};
    const std::vector<int> groups = outputChoices(project(), index_);
    if (!groups.empty())
        items.push_back(MenuItem::divider());
    for (int g : groups) {
        const Track &group = project().tracks[static_cast<size_t>(g)];
        items.push_back(choice(group.name, group.id));
    }
    if (groups.empty())
        items.push_back(MenuItem::item("No groups to go into", [] {}).when([] { return false; }));
    // Also to: a send at 0 dB into each group it may feed besides its output (a send to the output group would play twice)
    const std::vector<int> also = alsoToChoices(project(), index_);
    if (!also.empty()) {
        items.push_back(MenuItem::divider());
        items.push_back(MenuItem::item("Also to", [] {}).when([] { return false; }));
        for (int g : also) {
            const Track &group = project().tracks[static_cast<size_t>(g)];
            const ObjectId id = group.id;
            items.push_back(MenuItem::toggle(group.name, [&store, index, id] {
                const Track *tr = store.project().track(index);
                return tr && findSend(*tr, id) != nullptr;
            }, [&store, index, id] {
                const Track *tr = store.project().track(index);
                setAlsoTo(store, index, id, tr && !findSend(*tr, id));
            }));
        }
    }
    const Rect r = output_->rect();
    openContextMenu(context_, std::move(items), {r.x, r.y + r.h + 4.0f});
}

void ChannelStrip::handleInput(const Input &input)
{
    if (const Track *t = project().track(index_)) {
        const double pan = writer_ ? trackValueAt(*t, audio::kPanParam, writer_->playhead()) : t->pan;
        pan_->setValue(static_cast<float>((pan + 1.0) * 0.5));
        output_->setTone(t->output != kNoId ? TextTone::Accent : TextTone::Secondary);
    }
    name_->setTone(project().selectedTrack == index_ ? TextTone::Accent : TextTone::Primary);
    const size_t i = static_cast<size_t>(index_);
    meter_->setLevel(i < levels_.size() ? levels_[i] : 0.0f);
    const bool outputClicked = input.pressed && output_->rect().contains(input.mouse);
    Panel::handleInput(input);
    if (input.pressed && rect_.contains(input.mouse) && project().selectedTrack != index_)
        store_.select(index_);
    if (outputClicked)
        openOutputMenu();
}

// MasterStrip

MasterStrip::MasterStrip(ProjectStore &store, audio::AudioSystem &audio, const StereoLevels &levels, AutomationWriter *writer)
    : Panel(Direction::Column), levels_(levels), monitor_(audio.masterGain())
{
    // What plays at the playhead: an automated master volume follows its lane
    auto playing = [&store, writer] {
        return writer ? masterVolumeAt(store.project(), writer->playhead()) : static_cast<double>(store.project().masterVolume);
    };
    setAlign(Align::Stretch);
    setMinSize({kStripWidth + 20.0f, 0.0f});

    add<Label>("Master", TextStyle::Caption).setAlignment(TextAlign::Center);
    add<LiveLabel>([&audio] { return audio.running() ? std::string("Output") : std::string("No output"); },
                   TextStyle::Caption, TextTone::Secondary)
        .setAlignment(TextAlign::Center);

    // The project's master volume
    auto &faders = centredRow(*this);
    faders.setAlign(Align::Stretch).setGrow(1.0f);
    faders.add<Fader>([playing] { return faderPosition(static_cast<float>(playing()), kMasterMaxGain); },
                      [&store, writer](float position) {
                          const float volume = faderGain(position, kMasterMaxGain);
                          if (writer && writer->touchMaster(volume))
                              return;
                          if (store.project().masterVolume != volume)
                              store.editMasterVolume() = volume;
                      })
        .setMaxGain(kMasterMaxGain)
        .setDefault(faderPosition(1.0f, kMasterMaxGain));
    for (auto &meter : meters_) {
        meter = &faders.add<LevelMeter>(Direction::Column);
        meter->setThickness(8.0f).setLength(60.0f);
    }

    add<LiveLabel>([playing] { return decibels(static_cast<float>(playing())) + " dB"; }, TextStyle::Caption, TextTone::Secondary)
        .setAlignment(TextAlign::Center);

    // What you hear: the monitoring gain, never part of the mix
    auto &out = centredRow(*this);
    out.setGap(6.0f);
    audio::AudioParam &gain = monitor_;
    out_ = &out.add<Knob>(faderPosition(gain.get(), kMonitorMaxGain),
                          [&gain](float position) { gain.set(faderGain(position, kMonitorMaxGain)); });
    out_->setDiameter(48.0f);
    out.add<LiveLabel>([&gain] { return "Out " + decibels(gain.get()); }, TextStyle::Caption, TextTone::Secondary);
}

void MasterStrip::handleInput(const Input &input)
{
    for (int c = 0; c < 2; ++c)
        meters_[c]->setLevel(levels_.peak[c]);
    out_->setValue(faderPosition(monitor_.get(), kMonitorMaxGain));
    Panel::handleInput(input);
}

// MixerView

// The sends into groups, by track id and group id, and the groups' short names: what the strips' extra
// send bars (and their labels) were made for
std::vector<uint64_t> groupSendShape(const Project &project)
{
    std::vector<uint64_t> shape;
    for (const Track &t : project.tracks) {
        for (const Send &send : t.sends) {
            const Track *group = project.findTrack(send.to);
            if (group && group->kind == TrackKind::Group)
                shape.push_back((static_cast<uint64_t>(t.id) << 32) | group->id);
        }
    }
    // The groups' short names: the bars' labels follow them (a rename, or a second group named alike)
    for (const Track &t : project.tracks) {
        if (t.kind != TrackKind::Group)
            continue;
        uint64_t h = 1469598103934665603ull;
        for (char ch : groupSendLabel(t.name))
            h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
        shape.push_back(h);
    }
    return shape;
}

MixerView::MixerView(ui::Context &context, ProjectStore &store, audio::AudioSystem &audio, const StereoLevels &levels,
                     const std::vector<float> &trackLevels, std::function<void(TrackKind)> onAddTrack,
                     AutomationWriter *writer, FoldState *folds)
    : Container(Direction::Row), context_(context), store_(store), trackLevels_(trackLevels),
      onAddTrack_(std::move(onAddTrack)), writer_(writer), ownFolds_(store), folds_(folds ? folds : &ownFolds_)
{
    setAlign(Align::Stretch);
    auto &scroll = add<StripScroller>();
    scroll.setGrow(1.0f);
    scroll.content().setAlign(Align::Start);
    strips_ = &scroll.content().add<Container>(Direction::Row);
    strips_->setAlign(Align::Stretch).setGrow(1.0f);
    add<MasterStrip>(store, audio, levels, writer);
    rebuild();
}

MixerView::~MixerView()
{
    *alive_ = false;
}

void MixerView::handleInput(const Input &input)
{
    // The strips are tree children, so a rebuild is posted for after this frame's input. The
    // revision is recorded at once, so it is posted once
    // Any edit() moves the structure; only a change in the tracks themselves (trackShape) rebuilds
    // A send to a group has a bar of its own, so the strips are made again when the groups a track sends to change
    if (store_.revision() != shownRevision_) {
        shownRevision_ = store_.revision();
        if (groupSendShape(project()) != shownSends_) {
            shownSends_ = groupSendShape(project());
            context_.post([this, alive = alive_] {
                if (*alive)
                    rebuild();
            });
        }
    }
    if (store_.structureRevision() != shownStructure_) {
        shownStructure_ = store_.structureRevision();
        if (trackShape(project()) != shownShape_) {
            shownShape_ = trackShape(project()); // posted once
            context_.post([this, alive = alive_] {
                if (*alive)
                    rebuild();
            });
        }
    }
    // Folding a group in either view hides or shows strips (and a selection of a hidden track, from
    // here or elsewhere, unfolds its groups even when the arrangement isn't open)
    folds_->revealSelection();
    if (folds_->version() != shownFold_) {
        shownFold_ = folds_->version();
        context_.post([this, alive = alive_] {
            if (*alive)
                rebuild();
        });
    }
    Container::handleInput(input);
}

size_t MixerView::stripCount() const
{
    return stripCount_;
}

void MixerView::rebuild()
{
    shownStructure_ = store_.structureRevision();
    shownShape_ = trackShape(project());
    shownFold_ = folds_->version();
    shownRevision_ = store_.revision();
    shownSends_ = groupSendShape(project());
    strips_->clear();
    const std::vector<char> hidden = hiddenTracks(project());
    stripCount_ = 0;
    for (size_t i = 0; i < project().tracks.size(); ++i) {
        if (hidden[i])
            continue;
        strips_->add<ChannelStrip>(context_, store_, static_cast<int>(i), trackLevels_, writer_, folds_);
        ++stripCount_;
    }
    // A menu of the kinds of track, opened from the button's bottom edge
    auto &add = strips_->add<Button>();
    Button *addRaw = &add;
    add.onClick([this, addRaw] {
        const Rect r = addRaw->rect();
        openContextMenu(context_,
                        {MenuItem::item("Instrument track", [this] { onAddTrack_(TrackKind::Instrument); }),
                         MenuItem::item("Audio track", [this] { onAddTrack_(TrackKind::Audio); }),
                         MenuItem::item("Return track", [this] { onAddTrack_(TrackKind::Return); }),
                         MenuItem::item("Group track", [this] { onAddTrack_(TrackKind::Group); })},
                        {r.x, r.y + r.h + 6.0f});
    });
    add.setIcon(Icon::Plus).setShape(Shape::Circle).setSize(Size::Small).setAlignSelf(Align::Center);
}

}
