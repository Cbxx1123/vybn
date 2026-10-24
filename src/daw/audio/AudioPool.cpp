#include "daw/audio/AudioPool.h"

#include <utility>

namespace daw {

AudioPool::AudioPool(env::Environment &env)
    : env_(env), resources_(env.find<env::ResourceManager>()), memory_(env.find<audio::MemoryManager>())
{
    if (resources_)
        resources_->add(files_);
}

AudioPool::~AudioPool()
{
    jobs_.wait();
    if (resources_)
        resources_->remove(files_);
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
    analyses_.erase(path); // a new sound under the same name
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
    for (auto it = errors_.begin(); it != errors_.end();) {
        if (paths.count(it->first))
            ++it;
        else
            it = errors_.erase(it);
    }
    for (auto it = analyses_.begin(); it != analyses_.end();) {
        if (paths.count(it->first))
            ++it;
        else
            it = analyses_.erase(it);
    }
}

std::shared_ptr<const audio::AudioAnalysis> AudioPool::analysis(const std::string &path)
{
    const auto found = analyses_.find(path);
    if (found != analyses_.end())
        return found->second;
    if (analysing_.count(path))
        return nullptr;
    std::shared_ptr<const audio::AudioData> data = get(path);
    if (!data)
        return nullptr;
    analysing_.insert(path);
    env::Environment &env = env_;
    env::runJob(env_, env::Lane::Background, [this, &env, data, path] {
        std::string error;
        std::shared_ptr<const audio::AudioAnalysis> result = audio::analyseAudio(*data, error);
        if (!result)
            result = std::make_shared<audio::AudioAnalysis>(); // nothing found: not asked again
        env.post([this, path, result] {
            analysing_.erase(path);
            analyses_[path] = result;
            ++version_;
        });
    }, jobs_);
    return nullptr;
}

std::shared_ptr<const audio::AudioAnalysis> AudioPool::analyseNow(const std::string &path)
{
    const auto found = analyses_.find(path);
    if (found != analyses_.end())
        return found->second;
    std::shared_ptr<const audio::AudioData> data = files_.find(path);
    std::string error;
    if (!data)
        data = loadNow(path, error);
    if (!data)
        return nullptr;
    std::shared_ptr<const audio::AudioAnalysis> result = audio::analyseAudio(*data, error);
    if (!result)
        result = std::make_shared<audio::AudioAnalysis>();
    analyses_[path] = result;
    ++version_;
    return result;
}

}
