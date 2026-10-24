#include "daw/audio/AudioPool.h"

#include <utility>

#include "daw/model/Warping.h"
#include "audio/Onsets.h"
#include "audio/Stretch.h"

namespace daw {

namespace {

// All of a sound's samples in memory: a streamed one is decoded again whole (a background thread's job)
std::shared_ptr<const audio::AudioData> wholeSound(std::shared_ptr<const audio::AudioData> data)
{
    if (!data || !data->streamed())
        return data;
    std::string error;
    return audio::loadAudioFile(data->path, error, nullptr);
}

}

AudioPool::AudioPool(env::Environment &env)
    : env_(env), resources_(env.find<env::ResourceManager>()), memory_(env.find<audio::MemoryManager>())
{
    if (resources_) {
        resources_->add(files_);
        resources_->add(warps_);
    }
}

AudioPool::~AudioPool()
{
    // Stretches still running stop at their next check; nothing they made is posted
    closing_->store(true);
    for (auto &[clip, running] : runningWarp_)
        running.cancel->store(true);
    jobs_.wait();
    if (resources_) {
        resources_->remove(files_);
        resources_->remove(warps_);
    }
}

std::shared_ptr<const audio::AudioData> AudioPool::get(const std::string &path)
{
    if (path.empty())
        return nullptr;
    if (auto found = files_.find(path))
        return found;
    if (loading_.count(path) || errors_.count(path))
        return nullptr;
    loading_.insert(path);
    env::Environment &env = env_;
    audio::MemoryManager *memory = memory_;
    env::runJob(env_, env::Lane::Background, [this, &env, memory, path] {
        std::string error;
        std::shared_ptr<const audio::AudioData> data = audio::loadAudioFile(path, error, memory);
        env.post([this, path, data, error] {
            loading_.erase(path);
            if (data)
                files_.put(path, data);
            else
                errors_[path] = error;
            ++version_;
        });
    }, jobs_);
    return nullptr;
}

std::string AudioPool::error(const std::string &path) const
{
    auto found = errors_.find(path);
    return found == errors_.end() ? std::string() : found->second;
}

void AudioPool::add(const std::string &path, std::shared_ptr<const audio::AudioData> data)
{
    files_.put(path, std::move(data));
    errors_.erase(path);
    ++version_;
}

std::shared_ptr<const audio::AudioData> AudioPool::loadNow(const std::string &path, std::string &error)
{
    std::shared_ptr<const audio::AudioData> data = audio::loadAudioFile(path, error, memory_);
    if (data)
        add(path, data);
    return data;
}

void AudioPool::addRecording(const std::string &path, double sampleRate, uint32_t channels, std::vector<float> samples)
{
    add(path, audio::makeAudioData(path, sampleRate, channels, std::move(samples), memory_));
}

void AudioPool::keepOnly(const std::set<std::string> &paths)
{
    files_.keepIf([&paths](const std::string &path) { return paths.count(path) > 0; });
    // A stretched sound's key is its file, '#', then its warp (which has no '#')
    warps_.keepIf([&paths](const std::string &key) { return paths.count(key.substr(0, key.rfind('#'))) > 0; });
    for (auto it = errors_.begin(); it != errors_.end();) {
        if (paths.count(it->first))
            ++it;
        else
            it = errors_.erase(it);
    }
    for (auto it = onsets_.begin(); it != onsets_.end();) {
        if (paths.count(it->first))
            ++it;
        else
            it = onsets_.erase(it);
    }
}

WarpedSound AudioPool::warped(const Clip &clip, const audio::TempoMap &timing)
{
    WarpedSound sound;
    if (!clip.warped() || !clip.hasAudio())
        return sound;
    const double anchor = renderAnchor(timing, clip);
    const std::string key = clip.audioFile + "#" + warpKey(timing, clip.warp, anchor);
    if (auto found = warps_.find(key)) {
        sound.data = found;
        sound.warp = clip.warp;
        sound.exact = true;
        lastWarp_[clip.id] = key;
        return sound;
    }
    if (files_.contains(clip.audioFile) && !warping_.count(key)) {
        WarpJob job{key, clip.audioFile, clip.warp, timing, anchor};
        auto running = runningWarp_.find(clip.id);
        if (running == runningWarp_.end()) {
            startWarp(clip.id, std::move(job));
        } else if (running->second.key != key) {
            // The clip changed again while its last sound was being made: that one goes, this one next
            running->second.cancel->store(true);
            queuedWarp_[clip.id] = std::move(job);
        }
    }
    // Meanwhile, the last one made for the clip
    auto last = lastWarp_.find(clip.id);
    if (last != lastWarp_.end()) {
        if (auto found = warps_.find(last->second)) {
            sound.data = found;
            auto made = warpOf_.find(last->second);
            sound.warp = made != warpOf_.end() ? made->second : clip.warp;
        }
    }
    return sound;
}

void AudioPool::startWarp(ObjectId clip, WarpJob job)
{
    std::shared_ptr<const audio::AudioData> data = files_.find(job.path);
    if (!data)
        return;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    runningWarp_[clip] = WarpRunning{job.key, cancel};
    warping_.insert(job.key);
    env::Environment &env = env_;
    auto closing = closing_;
    env::runJob(env_, env::Lane::Background, [this, &env, clip, job, data, cancel, closing] {
        std::shared_ptr<const audio::AudioData> whole = wholeSound(data);
        std::shared_ptr<const audio::AudioData> made;
        if (whole && !whole->samples.empty() && !cancel->load()) {
            const audio::StretchSource source{whole->samples.data(), whole->frames, whole->channels, whole->sampleRate};
            std::vector<double> transients;
            if (job.warp.mode == audio::WarpMode::Beats || job.warp.mode == audio::WarpMode::Tonal)
                transients = audio::analyseOnsets(source.samples, source.frames, source.channels, source.sampleRate).transients;
            const audio::TimePath path = warpPath(job.timing, job.warp, job.anchor, whole->seconds());
            std::vector<float> out = audio::stretchAudio(source, path, job.warp.mode, job.warp.pitch(), transients, cancel.get());
            if (!out.empty() && !cancel->load())
                made = audio::makeAudioData("warp:" + job.path, whole->sampleRate, whole->channels, std::move(out));
        }
        if (closing->load())
            return;
        env.post([this, clip, job, made] {
            warping_.erase(job.key);
            auto running = runningWarp_.find(clip);
            if (running != runningWarp_.end() && running->second.key == job.key)
                runningWarp_.erase(running);
            if (made) {
                warps_.put(job.key, made);
                warpOf_[job.key] = job.warp;
                lastWarp_[clip] = job.key;
                ++version_;
            }
            // The newest asked for while this one was made
            auto queued = queuedWarp_.find(clip);
            if (queued != queuedWarp_.end()) {
                WarpJob next = std::move(queued->second);
                queuedWarp_.erase(queued);
                if (!warps_.contains(next.key) && !warping_.count(next.key))
                    startWarp(clip, std::move(next));
            }
        });
    }, jobs_);
}

const FileOnsets *AudioPool::onsets(const std::string &path)
{
    auto found = onsets_.find(path);
    if (found != onsets_.end())
        return &found->second;
    std::shared_ptr<const audio::AudioData> data = files_.find(path);
    if (!data || findingOnsets_.count(path))
        return nullptr;
    findingOnsets_.insert(path);
    env::Environment &env = env_;
    auto closing = closing_;
    env::runJob(env_, env::Lane::Background, [this, &env, path, data, closing] {
        FileOnsets result;
        std::shared_ptr<const audio::AudioData> whole = wholeSound(data);
        if (whole && !whole->samples.empty()) {
            const audio::OnsetAnalysis analysis =
                audio::analyseOnsets(whole->samples.data(), whole->frames, whole->channels, whole->sampleRate);
            result.transients = analysis.transients;
            result.tempo = audio::estimateTempo(analysis);
        }
        if (closing->load())
            return;
        env.post([this, path, result] {
            findingOnsets_.erase(path);
            onsets_[path] = result;
            ++version_;
        });
    }, jobs_);
    return nullptr;
}

}
