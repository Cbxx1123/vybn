#include "TransportBar.h"

#include <algorithm>
#include <string>

#include "Format.h"

namespace daw {

using namespace ui;

namespace {

double rateOf(audio::AudioSystem &audio)
{
    return audio.sampleRate() > 0 ? static_cast<double>(audio.sampleRate()) : 48000.0;
}

double samplesPerBeat(audio::AudioSystem &audio)
{
    return rateOf(audio) * 60.0 / audio.transport().tempo();
}

}

double transportBeats(audio::AudioSystem &audio)
{
    return static_cast<double>(audio.transport().position()) / samplesPerBeat(audio);
}

void seekBeats(audio::AudioSystem &audio, double beats)
{
    audio.transport().locate(static_cast<uint64_t>(std::max(0.0, beats) * samplesPerBeat(audio)));
}

void setLoopBeats(audio::AudioSystem &audio, double start, double end, bool on)
{
    const double perBeat = samplesPerBeat(audio);
    audio::Transport &transport = audio.transport();
    transport.setLoop(static_cast<uint64_t>(std::max(0.0, start) * perBeat), static_cast<uint64_t>(std::max(0.0, end) * perBeat));
    transport.setLooping(on && end > start);
}

TransportBar::TransportBar(ui::Context &context, audio::AudioSystem &audio, audio::Metronome &metronome, ProjectStore &store,
                           const StereoLevels &levels, RecordHooks record)
    : Panel(Direction::Row), audio_(audio), levels_(levels), record_(std::move(record))
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

    add<Readout>([&audio, &store] { return barsBeats(transportBeats(audio), store.project().beatsPerBar); },
                 [&audio] { return clockTime(static_cast<double>(audio.transport().position()) / rateOf(audio)); },
                 "000.0.000");

    auto &tempo = add<Container>(Direction::Column);
    tempo.setAlign(Align::Start);
    tempo.add<Label>("Tempo", TextStyle::Caption, TextTone::Secondary);
    tempo_ = &tempo.add<Stepper>(static_cast<float>(transport.tempo()), 40.0f, 300.0f, 1.0f, [&transport](float bpm) {
        transport.setTempo(bpm);
    }).setSuffix(" BPM").setSize(Size::Small);

    auto &signature = add<Container>(Direction::Column);
    signature.setAlign(Align::Start);
    signature.add<Label>("Time", TextStyle::Caption, TextTone::Secondary);
    signature.add<LiveLabel>([&store] { return std::to_string(store.project().beatsPerBar) + " / 4"; }, TextStyle::Subtitle);

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
    tempo_->setValue(static_cast<float>(audio_.transport().tempo())); // a loaded project changes it
    for (int c = 0; c < 2; ++c)
        meters_[c]->setLevel(levels_.peak[c]);
    Panel::handleInput(input);
}

}
