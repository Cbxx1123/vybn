#include "NotePlayer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace audio {

void NoteSequence::sort()
{
    std::stable_sort(notes.begin(), notes.end(), [](const SequencedNote &a, const SequencedNote &b) { return a.start < b.start; });
}

float ControlCurve::valueAt(double beat) const
{
    if (points.empty())
        return 0.0f;
    if (beat <= points.front().beat)
        return points.front().value;
    if (beat >= points.back().beat)
        return points.back().value;
    const auto next = std::upper_bound(points.begin(), points.end(), beat, [](double b, const Point &p) { return b < p.beat; });
    const Point &b = *next;
    const Point &a = *(next - 1);
    if (stepped || b.beat <= a.beat)
        return a.value;
    return a.value + (b.value - a.value) * static_cast<float>((beat - a.beat) / (b.beat - a.beat));
}

NotePlayer::NotePlayer(std::unique_ptr<Instrument> instrument)
    : AudioProcessor(instrument->name()), instrument_(std::move(instrument))
{
    sent_.fill(-1.0f);
}

NotePlayer::~NotePlayer()
{
    // The audio thread has let go of everything by now
    delete current_;
    NoteSequence *s = nullptr;
    while (incoming_.pop(s))
        delete s;
    while (retired_.pop(s))
        delete s;
}

bool NotePlayer::setSequence(std::unique_ptr<NoteSequence> sequence)
{
    if (sequence)
        sequence->sort();
    NoteSequence *raw = sequence.release();
    if (incoming_.push(raw))
        return true;
    delete raw;
    return false;
}

void NotePlayer::audition(int key, float velocity)
{
    live_.push({key, std::max(0.01f, velocity)});
}

void NotePlayer::release(int key)
{
    live_.push({key, 0.0f});
}

void NotePlayer::collect()
{
    NoteSequence *s = nullptr;
    while (retired_.pop(s))
        delete s;
    instrument_->collect();
}

void NotePlayer::prepare(double sampleRate, uint32_t maxFrames)
{
    instrument_->prepare(sampleRate, maxFrames);
    soundingCount_ = 0;
    wasPlaying_ = false;
}

void NotePlayer::allOff()
{
    instrument_->allNotesOff();
    soundingCount_ = 0;
    resetControls();
}

void NotePlayer::resetControls()
{
    // The bend back to the middle and the pedal up; values are sent again from the next block
    if (sent_[Instrument::kPitchBend] >= 0.0f && sent_[Instrument::kPitchBend] != 0.5f)
        instrument_->controlChange(Instrument::kPitchBend, 0.5f);
    sent_.fill(-1.0f);
    pedal_ = false;
    pedalled_.reset();
}

void NotePlayer::playControls(double beat)
{
    if (!current_)
        return;
    for (const ControlCurve &curve : current_->controls) {
        if (curve.controller < 0 || curve.controller >= Instrument::kControllers || curve.points.empty())
            continue;
        // In MIDI's steps (the bend's are finer), so a still curve isn't sent again
        const float steps = curve.controller == Instrument::kPitchBend ? 8191.0f : 127.0f;
        const float value = std::round(std::clamp(curve.valueAt(beat), 0.0f, 1.0f) * steps) / steps;
        float &last = sent_[static_cast<size_t>(curve.controller)];
        if (value == last)
            continue;
        last = value;
        if (curve.controller == 64) {
            pedal_ = value >= 0.5f;
            if (!pedal_) {
                for (size_t k = 0; k < 128; ++k) {
                    if (pedalled_.test(k))
                        instrument_->noteOff(static_cast<int>(k));
                }
                pedalled_.reset();
            }
        } else {
            instrument_->controlChange(curve.controller, value);
        }
    }
}

void NotePlayer::render(AudioBlock &block, uint32_t from, uint32_t to)
{
    if (to <= from)
        return;
    float *channels[2] = {block.channels[0] + from, block.channelCount > 1 ? block.channels[1] + from : nullptr};
    AudioBlock part{channels, std::min<uint32_t>(block.channelCount, 2), to - from, block.sampleRate, block.transport};
    instrument_->process(part);
}

void NotePlayer::playMidi(const MidiMessage &m)
{
    const int key = m.data1;
    if (m.isNoteOn()) {
        instrument_->noteOn(key, m.data2 / 127.0f);
        held_.set(static_cast<size_t>(key));
        sustained_.reset(static_cast<size_t>(key));
    } else if (m.isNoteOff()) {
        held_.reset(static_cast<size_t>(key));
        if (sustain_)
            sustained_.set(static_cast<size_t>(key));
        else
            instrument_->noteOff(key);
    } else if (m.isController(64)) {
        sustain_ = m.data2 >= 64;
        if (!sustain_) {
            for (size_t k = 0; k < 128; ++k) {
                if (sustained_.test(k))
                    instrument_->noteOff(static_cast<int>(k));
            }
            sustained_.reset();
        }
    } else if (m.isController(123) || m.isController(120)) {
        releaseMidi(); // all notes off, all sound off
    } else if (m.type() == 0xB0) {
        instrument_->controlChange(m.data1, m.data2 / 127.0f);
    } else if (m.type() == 0xE0) {
        instrument_->controlChange(Instrument::kPitchBend, static_cast<float>(m.data1 | (m.data2 << 7)) / 16383.0f);
    }
}

void NotePlayer::releaseMidi()
{
    for (size_t k = 0; k < 128; ++k) {
        if (held_.test(k) || sustained_.test(k))
            instrument_->noteOff(static_cast<int>(k));
    }
    held_.reset();
    sustained_.reset();
    sustain_ = false;
}

void NotePlayer::process(AudioBlock &block)
{
    NoteSequence *next = nullptr;
    // A new sequence. Only take one when the old one can be handed back, since it can't be
    // freed here; the main thread empties the retired queue every frame.
    while (!retired_.full() && incoming_.pop(next)) {
        if (current_)
            retired_.push(current_);
        current_ = next;
    }

    instrument_->automateBlock(block.transport);
    instrument_->beginBlock();

    // Previewed keys sound at once
    Live live{};
    while (live_.pop(live)) {
        if (live.velocity > 0.0f)
            instrument_->noteOn(live.key, live.velocity);
        else
            instrument_->noteOff(live.key);
    }

    // A MIDI keyboard, while this track listens
    const bool listening = midi_ && listen.load(std::memory_order_relaxed);
    if (listening) {
        for (size_t i = 0; i < midi_->eventCount(); ++i)
            playMidi(midi_->events()[i]);
    } else if (wasListening_) {
        releaseMidi();
    }
    wasListening_ = listening;

    const TransportState &t = block.transport;
    if (!t.playing) {
        if (wasPlaying_)
            allOff();
        wasPlaying_ = false;
        render(block, 0, block.frames);
        return;
    }
    if (wasPlaying_ && t.position != expected_)
        allOff(); // the transport jumped
    wasPlaying_ = true;
    expected_ = t.position + block.frames;

    const double b0 = t.beatAt(0);
    const double b1 = t.beatAt(block.frames);
    playControls(b0);
    auto frameOf = [&](double beat) {
        const double f = std::floor(t.frameAt(beat) + 0.5);
        return static_cast<uint32_t>(std::clamp(f, 0.0, static_cast<double>(block.frames - 1)));
    };

    size_t count = 0;
    // Sounding notes that end in this block
    for (size_t i = 0; i < soundingCount_;) {
        if (sounding_[i].end < b1) {
            if (count < kMaxEvents)
                events_[count++] = {frameOf(sounding_[i].end), false, sounding_[i].key, 0.0f};
            sounding_[i] = sounding_[--soundingCount_];
        } else {
            ++i;
        }
    }
    // Notes that start in this block
    if (current_) {
        const auto &notes = current_->notes;
        auto it = std::lower_bound(notes.begin(), notes.end(), b0,
                                   [](const SequencedNote &n, double beat) { return n.start < beat; });
        for (; it != notes.end() && it->start < b1 && count + 2 <= kMaxEvents; ++it) {
            if (it->end <= it->start)
                continue;
            if (it->end < b1) {
                events_[count++] = {frameOf(it->start), true, it->key, it->velocity};
                events_[count++] = {std::max(frameOf(it->start), frameOf(it->end)), false, it->key, 0.0f};
            } else if (soundingCount_ < kMaxSounding) {
                events_[count++] = {frameOf(it->start), true, it->key, it->velocity};
                sounding_[soundingCount_++] = {it->key, it->end};
            }
        }
    }

    // In frame order; at the same frame, endings before starts, so a repeated key restarts
    std::stable_sort(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(count), [](const Event &a, const Event &b) {
        if (a.frame != b.frame)
            return a.frame < b.frame;
        return !a.on && b.on;
    });

    // While the sequence's pedal is down its note offs wait for it to lift
    if (pedal_) {
        for (size_t i = 0; i < count; ++i) {
            Event &e = events_[i];
            if (!e.on && e.key >= 0 && e.key < 128) {
                pedalled_.set(static_cast<size_t>(e.key));
                e.key = -1;
            } else if (e.on && e.key >= 0 && e.key < 128) {
                pedalled_.reset(static_cast<size_t>(e.key));
            }
        }
    }

    // An instrument that takes timed notes gets them all, then the whole block
    if (instrument_->timedEvents()) {
        for (size_t i = 0; i < count; ++i) {
            const Event &e = events_[i];
            if (e.key < 0)
                continue;
            if (e.on)
                instrument_->noteOnAt(e.key, e.velocity, e.frame);
            else
                instrument_->noteOffAt(e.key, e.frame);
        }
        render(block, 0, block.frames);
        return;
    }

    uint32_t cursor = 0;
    for (size_t i = 0; i < count; ++i) {
        const Event &e = events_[i];
        if (e.key < 0)
            continue;
        render(block, cursor, e.frame);
        cursor = std::max(cursor, e.frame);
        if (e.on)
            instrument_->noteOn(e.key, e.velocity);
        else
            instrument_->noteOff(e.key);
    }
    render(block, cursor, block.frames);
}

}
