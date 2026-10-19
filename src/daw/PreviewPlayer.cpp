#include "PreviewPlayer.h"

#include <algorithm>
#include <set>
#include <utility>

#include "AudioPool.h"
#include "Library.h"
#include "audio/AudioSystem.h"
#include "audio/Sampler.h"
#include "audio/Synth.h"
#include "engine/Window.h"

namespace daw {

namespace {

audio::Sampler &samplerOf(audio::NotePlayer &player)
{
    return static_cast<audio::Sampler &>(player.instrument());
}

}

PreviewPlayer::PreviewPlayer(audio::AudioSystem &audio, InstrumentLibrary &library, AudioPool &pool, engine::Window &window,
                             std::function<void(const std::string &)> notify)
    : soundPreview_(audio.add<audio::SoundPreview>()),
      previewPlayer_(audio.add<audio::NotePlayer>(std::make_unique<audio::Sampler>("Preview"))),
      synthPlayer_(audio.add<audio::NotePlayer>(std::make_unique<audio::SynthDevice>(audio::SynthDevice::setting()))),
      library_(library), pool_(pool), window_(window), notify_(std::move(notify))
{
    samplerOf(previewPlayer_).gain.set(0.5f);
    synth_ = &static_cast<audio::SynthDevice &>(synthPlayer_.instrument());
}

void PreviewPlayer::previewInstrument(const std::string &name)
{
    stopPreview();
    previewName_ = name;
    previewLoading_ = true;
    library_.load(name, [this, name](std::shared_ptr<const audio::SampleSet> set, const std::string &error) {
        if (previewName_ != name)
            return; // stopped, or another one asked for since
        previewLoading_ = false;
        if (!set) {
            notify_("Couldn't load \"" + name + "\": " + error);
            previewName_.clear();
            return;
        }
        previewSet_ = std::move(set);
    });
}

void PreviewPlayer::previewSound(const std::string &path)
{
    stopPreview();
    previewPath_ = path;
}

void PreviewPlayer::previewSynth(const audio::DeviceSetting &setting)
{
    stopPreview();
    synth_->applySetting(setting);
    phrase_ = chordPhrase(48);
    phraseStart_ = window_.time();
    phraseNext_ = 0;
    phraseOnSynth_ = true;
}

void PreviewPlayer::stopPreview()
{
    soundPreview_.stop();
    audio::NotePlayer &target = phraseOnSynth_ ? synthPlayer_ : previewPlayer_;
    for (const PhraseStep &s : phrase_)
        target.release(s.key);
    phrase_.clear();
    phraseOnSynth_ = false;
    previewName_.clear();
    previewPath_.clear();
    previewSet_ = nullptr;
    previewLoading_ = false;
}

bool PreviewPlayer::previewing() const
{
    return soundPreview_.playing() || !phrase_.empty() || previewLoading_ || !previewPath_.empty() || previewSet_;
}

void PreviewPlayer::update()
{
    const double now = window_.time();
    soundPreview_.collect();
    previewPlayer_.collect();
    synthPlayer_.collect();
    samplerOf(previewPlayer_).collect();

    // A sound file, once the pool has it
    if (!previewPath_.empty()) {
        if (auto data = pool_.get(previewPath_)) {
            if (soundPreview_.play(data))
                previewPath_.clear();
        } else if (!pool_.loading(previewPath_)) {
            notify_("Couldn't play it: " + pool_.error(previewPath_));
            previewPath_.clear();
        }
    }

    // An instrument, once loaded: into the preview sampler, then a short phrase
    if (previewSet_ && samplerOf(previewPlayer_).setSampleSet(previewSet_)) {
        const audio::SampleSet &set = *previewSet_;
        phrase_.clear();
        phraseOnSynth_ = false;
        const LibraryInstrument *entry = library_.find(previewName_);
        const bool drums = entry && (entry->category == "Drums" || entry->category == "Percussion");
        if (drums) {
            // A few of its sounds in turn: the first keys it has
            std::set<int> keys;
            for (const audio::SampleZone &z : set.zones)
                keys.insert(z.lowKey);
            int i = 0;
            for (int k : keys) {
                if (i >= 8)
                    break;
                phrase_.push_back({i * 0.22, k, 0.85f, true});
                phrase_.push_back({i * 0.22 + 0.2, k, 0.0f, false});
                ++i;
            }
        } else {
            // An arpeggio and a chord around the middle of its range
            int low = 127, high = 0;
            for (const audio::SampleZone &z : set.zones) {
                low = std::min(low, z.rootKey);
                high = std::max(high, z.rootKey);
            }
            phrase_ = chordPhrase(std::clamp(48, std::min(low, 60), std::max(48, high - 12)));
        }
        std::stable_sort(phrase_.begin(), phrase_.end(), [](const PhraseStep &a, const PhraseStep &b) { return a.at < b.at; });
        phraseStart_ = now;
        phraseNext_ = 0;
        previewSet_ = nullptr;
    }
    audio::NotePlayer &target = phraseOnSynth_ ? synthPlayer_ : previewPlayer_;
    while (phraseNext_ < phrase_.size() && phrase_[phraseNext_].at <= now - phraseStart_) {
        const PhraseStep &s = phrase_[phraseNext_++];
        if (s.on)
            target.audition(s.key, s.velocity);
        else
            target.release(s.key);
    }
    // Done once the last note has had time to ring out
    if (!phrase_.empty() && phraseNext_ >= phrase_.size() && now - phraseStart_ > phrase_.back().at + 1.0) {
        // The synthesizer has no sampler to let go of: its voices end on their own
        if (!phraseOnSynth_) {
            previewName_.clear();
            previewHeld_ = true;
        }
        phrase_.clear();
        phraseOnSynth_ = false;
    }
    // Then the sampler lets go of the instrument, so nothing holds it and it can be freed
    if (previewHeld_ && !previewSet_ && phrase_.empty() && samplerOf(previewPlayer_).setSampleSet(nullptr))
        previewHeld_ = false;
}

std::vector<PreviewPlayer::PhraseStep> PreviewPlayer::chordPhrase(int root)
{
    std::vector<PhraseStep> phrase;
    const int steps[] = {0, 4, 7, 12};
    for (int i = 0; i < 4; ++i) {
        phrase.push_back({i * 0.18, root + steps[i], 0.75f, true});
        phrase.push_back({i * 0.18 + 0.17, root + steps[i], 0.0f, false});
    }
    for (int k : {0, 4, 7})
        phrase.push_back({0.8, root + k, 0.7f, true});
    for (int k : {0, 4, 7})
        phrase.push_back({2.0, root + k, 0.0f, false});
    return phrase;
}

}
