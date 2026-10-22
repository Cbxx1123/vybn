#include "daw/views/TransportBar.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include "daw/widgets/Format.h"

namespace daw {

using namespace ui;

namespace {

double rateOf(audio::AudioSystem &audio)
{
    return audio.sampleRate() > 0 ? static_cast<double>(audio.sampleRate()) : 48000.0;
}

}

double transportBeats(audio::AudioSystem &audio)
{
    return audio.transport().beats(rateOf(audio));
}

void seekBeats(audio::AudioSystem &audio, double beats)
{
    audio.transport().locate(audio.transport().sampleOf(beats, rateOf(audio)));
}

void setLoopBeats(audio::AudioSystem &audio, double start, double end, bool on)
{
    audio::Transport &transport = audio.transport();
    transport.setLoop(start, end);
    transport.setLooping(on && end > start);
}

TransportBar::TransportBar(ui::Context &context, audio::AudioSystem &audio, audio::Metronome &metronome, ProjectStore &store,
                           const StereoLevels &levels, RecordHooks record, std::function<void()> editSignature)
    : Panel(Direction::Row), audio_(audio), store_(store), levels_(levels), record_(std::move(record)),
      editSignature_(std::move(editSignature))
{
    setAlign(Align::Center);
    setDepth(Depth::Normal);
    audio::Transport &transport = audio.transport();

    add<Button>([&transport] { transport.locate(0); }).setIcon(Icon::Previous).setShape(Shape::Circle).setSize(Size::Small);
    play_ = &add<Button>([&transport] { transport.toggle(); });
    play_->setIcon(Icon::Play).setShape(Shape::Circle).setVariant(ButtonVariant::Accent).setFinish(Finish::Convex);
    // Stop; pressed again while stopped, it returns to the start
    add<Button>([&transport] {
        if (!transport.playing())
            transport.locate(0);
        transport.stop();
    }).setIcon(Icon::Stop).setShape(Shape::Circle).setSize(Size::Small);
    // Record: onto the armed tracks
    recordButton_ = &add<Button>([this] {
        if (record_.toggle)
            record_.toggle();
    });
    recordButton_->setIcon(Icon::Dot).setShape(Shape::Circle).setSize(Size::Small);

    add<Readout>([&audio, &store] { return barsBeats(transportBeats(audio), store.project().timing); },
                 [&audio] { return clockTime(static_cast<double>(audio.transport().position()) / rateOf(audio)); },
                 "000.0.000");

    auto &tempo = add<Container>(Direction::Column);
    tempo.setAlign(Align::Start);
    tempo.add<Label>("Tempo", TextStyle::Caption, TextTone::Secondary);
    // The tempo at the playhead; changing it changes the tempo point in force there (with one point,
    // the song's tempo)
    auto setTempo = [&audio, &store](double bpm) {
        audio::TempoMap timing = store.project().timing;
        std::vector<audio::TempoPoint> points = timing.points();
        points[timing.pointAt(transportBeats(audio))].bpm = bpm;
        timing.setPoints(std::move(points));
        store.setTiming(timing);
    };
    auto &tempoRow = tempo.add<Container>(Direction::Row);
    tempoRow.setAlign(Align::Center);
    tempo_ = &tempoRow.add<Stepper>(static_cast<float>(store.project().timing.tempo()), 20.0f, 400.0f, 1.0f,
                                    [setTempo](float bpm) { setTempo(bpm); });
    tempo_->setSuffix(" BPM").setSize(Size::Small);
    // Tap: the tempo from the last few taps (a pause of two seconds starts again), from the second on
    tempoRow.add<Button>([this, setTempo] {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!taps_.empty() && now - taps_.back() > 2.0)
            taps_.clear();
        taps_.push_back(now);
        if (taps_.size() > 8)
            taps_.erase(taps_.begin());
        if (taps_.size() >= 2) {
            const double beat = (taps_.back() - taps_.front()) / static_cast<double>(taps_.size() - 1);
            setTempo(std::clamp(std::round(60.0 / beat * 10.0) / 10.0, audio::TempoMap::kMinBpm, audio::TempoMap::kMaxBpm));
        }
    }).setLabel("Tap").setSize(Size::Small);

    // The signature at the playhead: pressing it opens its editor
    auto &signature = add<Container>(Direction::Column);
    signature.setAlign(Align::Start);
    signature.add<Label>("Time", TextStyle::Caption, TextTone::Secondary);
    signature_ = &signature.add<Button>([this] {
        if (editSignature_)
            editSignature_();
    });
    signature_->setLabel("4 / 4").setSize(Size::Small);

    // The section the playhead is in: the marker at or before it
    auto &section = add<Container>(Direction::Column);
    section.setAlign(Align::Start);
    section.add<Label>("Section", TextStyle::Caption, TextTone::Secondary);
    section.add<LiveLabel>([&audio, &store] {
        const int at = store.project().markerAt(transportBeats(audio));
        return at >= 0 ? store.project().markers[static_cast<size_t>(at)].name : std::string("-");
    }, TextStyle::Subtitle).setMinSize({90.0f, 0.0f});

    auto &click = add<Container>(Direction::Column);
    click.setAlign(Align::Start);
    click.add<Label>("Playback", TextStyle::Caption, TextTone::Secondary);
    auto &clickRow = click.add<Container>(Direction::Row);
    clickRow.add<SmallToggle>(
        "Click", [&metronome] { return metronome.enabled.load(); }, [&metronome](bool on) { metronome.enabled = on; });
    // The loop (set on the arrangement's or the piano roll's ruler)
    clickRow.add<SmallToggle>("Loop", [&store] { return store.project().looping; }, [&store](bool on) {
        if (on != store.project().looping)
            store.toggleLoop();
    });

    // Clicks before a take starts, so the take comes in on the beat
    if (record_.countIn && record_.setCountIn) {
        auto &count = add<Container>(Direction::Column);
        count.setAlign(Align::Start);
        count.add<Label>("Count-in", TextStyle::Caption, TextTone::Secondary);
        count.add<Dropdown>(context, std::vector<std::string>{"Off", "1 beat", "1 bar", "2 bars"}, record_.countIn(),
                            [this](int choice) { record_.setCountIn(choice); })
            .setSize(Size::Small);
    }

    add<Spacer>();

    // The inputs in use
    auto &inputs = add<Container>(Direction::Column);
    inputs.setAlign(Align::Start);
    inputs.add<Label>("Inputs", TextStyle::Caption, TextTone::Secondary);
    auto &midiRow = inputs.add<Container>(Direction::Row);
    midiRow.setAlign(Align::Center);
    midiRow.add<ActivityLight>(record_.midiActive);
    auto midiText = record_.midiInput;
    midiRow.add<LiveLabel>([midiText] {
        const std::string name = midiText ? midiText() : std::string();
        return "MIDI: " + (name.empty() ? std::string("none plugged in") : name);
    }, TextStyle::Caption);
    auto &audioRow = inputs.add<Container>(Direction::Row);
    audioRow.setAlign(Align::Center);
    inputMeter_ = &audioRow.add<LevelMeter>(Direction::Row);
    inputMeter_->setThickness(5.0f).setLength(60.0f);
    auto audioText = record_.audioInput;
    audioRow.add<LiveLabel>([audioText] { return "Audio: " + (audioText ? audioText() : std::string()); },
                            TextStyle::Caption);
    auto notice = record_.notice;
    inputs.add<LiveLabel>([notice] { return notice ? notice() : std::string(); }, TextStyle::Caption, TextTone::Accent);

    auto &output = add<Container>(Direction::Column);
    output.setAlign(Align::Stretch);
    output.add<Label>("Output", TextStyle::Caption, TextTone::Secondary);
    for (auto &meter : meters_) {
        meter = &output.add<LevelMeter>(Direction::Row);
        meter->setThickness(7.0f).setLength(200.0f);
    }
}

void TransportBar::handleInput(const Input &input)
{
    play_->setIcon(audio_.transport().playing() ? Icon::Pause : Icon::Play);
    const bool recording = record_.recording && record_.recording();
    recordButton_->setVariant(recording ? ButtonVariant::Accent : ButtonVariant::Raised);
    inputMeter_->setLevel(record_.inputLevel ? record_.inputLevel() : 0.0f);
    // The tempo and signature where the playhead is (it moves, and a loaded project or an undo changes them)
    const double at = transportBeats(audio_);
    tempo_->setValue(static_cast<float>(store_.project().timing.bpmAt(at)));
    const audio::Signature &s = store_.project().timing.signatureAt(at);
    signature_->setLabel(std::to_string(s.numerator) + " / " + std::to_string(s.denominator));
    for (int c = 0; c < 2; ++c)
        meters_[c]->setLevel(levels_.peak[c]);
    Panel::handleInput(input);
}

}
