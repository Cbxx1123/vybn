#include "Mixer.h"

#include <string>
#include <utility>

#include "Format.h"

namespace daw {

using namespace ui;

namespace {

constexpr float kStripWidth = 112.0f;
constexpr float kTrackMaxGain = 2.0f;  // +6 dB
constexpr float kMasterMaxGain = 1.5f; // the AudioSystem's master gain range

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

}

// ChannelStrip

ChannelStrip::ChannelStrip(ProjectStore &store, int index, const std::vector<float> &levels)
    : Panel(Direction::Column), store_(store), index_(index), levels_(levels)
{
    setAlign(Align::Stretch);
    setDepth(Depth::Normal);
    setMinSize({kStripWidth, 0.0f});

    auto trackName = [&store, index] {
        const Track *t = store.project().track(index);
        return t ? t->name : std::string();
    };
    name_ = &add<LiveLabel>(trackName, TextStyle::Caption);
    name_->setAlignment(TextAlign::Center);

    pan_ = &add<Knob>(0.5f, [&store, index](float v) {
        if (store.project().track(index))
            store.editMix(index).pan = v * 2.0f - 1.0f;
    });
    pan_->setDiameter(46.0f).setAlignSelf(Align::Center);

    auto &faders = centredRow(*this);
    faders.setAlign(Align::Stretch).setGrow(1.0f);
    faders.add<Fader>(
              [&store, index] {
                  const Track *t = store.project().track(index);
                  return t ? faderPosition(t->gain, kTrackMaxGain) : 0.0f;
              },
              [&store, index](float position) {
                  if (store.project().track(index))
                      store.editMix(index).gain = faderGain(position, kTrackMaxGain);
              })
        .setMaxGain(kTrackMaxGain)
        .setDefault(faderPosition(1.0f, kTrackMaxGain));
    meter_ = &faders.add<LevelMeter>(Direction::Column);
    meter_->setThickness(8.0f).setLength(60.0f);

    add<LiveLabel>([&store, index] {
        const Track *t = store.project().track(index);
        if (!t)
            return std::string();
        return decibels(t->gain) + "  " + panText(t->pan);
    }, TextStyle::Caption, TextTone::Secondary).setAlignment(TextAlign::Center);

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
    toggle("R", &Track::armed);
}

void ChannelStrip::handleInput(const Input &input)
{
    if (const Track *t = project().track(index_))
        pan_->setValue((t->pan + 1.0f) * 0.5f);
    name_->setTone(project().selectedTrack == index_ ? TextTone::Accent : TextTone::Primary);
    const size_t i = static_cast<size_t>(index_);
    meter_->setLevel(i < levels_.size() ? levels_[i] : 0.0f);
    Panel::handleInput(input);
    if (input.pressed && rect_.contains(input.mouse) && project().selectedTrack != index_)
        store_.select(index_);
}

// MasterStrip

MasterStrip::MasterStrip(audio::AudioSystem &audio, const StereoLevels &levels)
    : Panel(Direction::Column), levels_(levels)
{
    setAlign(Align::Stretch);
    setMinSize({kStripWidth + 20.0f, 0.0f});

    add<Label>("Master", TextStyle::Caption).setAlignment(TextAlign::Center);
    add<LiveLabel>([&audio] { return audio.running() ? std::string("Output") : std::string("No output"); },
                   TextStyle::Caption, TextTone::Secondary)
        .setAlignment(TextAlign::Center);

    auto &faders = centredRow(*this);
    faders.setAlign(Align::Stretch).setGrow(1.0f);
    audio::AudioParam &gain = audio.masterGain();
    faders.add<Fader>([&gain] { return faderPosition(gain.get(), kMasterMaxGain); },
                      [&gain](float position) { gain.set(faderGain(position, kMasterMaxGain)); })
        .setMaxGain(kMasterMaxGain)
        .setDefault(faderPosition(0.8f, kMasterMaxGain));
    for (auto &meter : meters_) {
        meter = &faders.add<LevelMeter>(Direction::Column);
        meter->setThickness(8.0f).setLength(60.0f);
    }

    add<LiveLabel>([&gain] { return decibels(gain.get()) + " dB"; }, TextStyle::Caption, TextTone::Secondary)
        .setAlignment(TextAlign::Center);
}

void MasterStrip::handleInput(const Input &input)
{
    for (int c = 0; c < 2; ++c)
        meters_[c]->setLevel(levels_.peak[c]);
    Panel::handleInput(input);
}

// MixerView

MixerView::MixerView(ui::Context &context, ProjectStore &store, audio::AudioSystem &audio, const StereoLevels &levels,
                     const std::vector<float> &trackLevels, std::function<void(TrackKind)> onAddTrack)
    : Container(Direction::Row), context_(context), store_(store), trackLevels_(trackLevels),
      onAddTrack_(std::move(onAddTrack))
{
    setAlign(Align::Stretch);
    auto &scroll = add<StripScroller>();
    scroll.setGrow(1.0f);
    scroll.content().setAlign(Align::Start);
    strips_ = &scroll.content().add<Container>(Direction::Row);
    strips_->setAlign(Align::Stretch).setGrow(1.0f);
    add<MasterStrip>(audio, levels);
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
    Container::handleInput(input);
}

void MixerView::rebuild()
{
    shownStructure_ = store_.structureRevision();
    shownShape_ = trackShape(project());
    strips_->clear();
    for (size_t i = 0; i < project().tracks.size(); ++i)
        strips_->add<ChannelStrip>(store_, static_cast<int>(i), trackLevels_);
    // A menu of the two kinds of track, opened from the button's bottom edge
    auto &add = strips_->add<Button>();
    Button *addRaw = &add;
    add.onClick([this, addRaw] {
        const Rect r = addRaw->rect();
        openContextMenu(context_,
                        {MenuItem::item("Instrument track", [this] { onAddTrack_(TrackKind::Instrument); }),
                         MenuItem::item("Audio track", [this] { onAddTrack_(TrackKind::Audio); })},
                        {r.x, r.y + r.h + 6.0f});
    });
    add.setIcon(Icon::Plus).setShape(Shape::Circle).setSize(Size::Small).setAlignSelf(Align::Center);
}

}
