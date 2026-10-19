#include "AudioSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <miniaudio.h>

#include "env/Scheduler.h"

#if defined(__SSE__) || defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h>
#define AUDIO_HAS_SSE 1
#endif

namespace audio {

namespace {

constexpr size_t kMaxProcessors = 256;

// Very small numbers ("denormals") are slow on x86; flush them to zero on the audio thread
void disableDenormals()
{
#ifdef AUDIO_HAS_SSE
    _mm_setcsr(_mm_getcsr() | 0x8040); // flush-to-zero and denormals-are-zero
#endif
}

}

struct AudioSystem::Impl {
    ma_context context{};
    bool contextReady = false;
    ma_device device{};
    bool deviceReady = false;
    std::vector<ma_device_id> ids;      // matches devices_
    std::vector<ma_device_id> inputIds; // matches inputDevices_
    std::atomic<bool> stopping{false};   // a stop we asked for, so its notification isn't a failure
    double processorsRate = 0.0;         // the rate processors were last prepared for
};

AudioSystem::AudioSystem(env::Environment &env, AudioSettings settings)
    : env::Daemon("Audio"), env_(env), impl_(std::make_unique<Impl>()), settings_(settings)
{
    reserveBuffers();
    // Tracks render across the cores when the program has a scheduler (tests may not)
    if (env::Scheduler *scheduler = env.find<env::Scheduler>())
        pool_ = &scheduler->realtime();

    ma_context_config config = ma_context_config_init();
#ifdef _WIN32
    const ma_backend backends[] = {ma_backend_wasapi};
    const ma_result result = ma_context_init(backends, 1, &config, &impl_->context);
#else
    const ma_result result = ma_context_init(nullptr, 0, &config, &impl_->context);
#endif
    if (result != MA_SUCCESS) {
        lastError_ = std::string("No audio backend: ") + ma_result_description(result);
        return;
    }
    impl_->contextReady = true;
    backendName_ = ma_get_backend_name(impl_->context.backend);
    refreshDevices();
}

AudioSystem::AudioSystem(env::Environment &env, Deviceless)
    : env::Daemon("Audio"), env_(env), impl_(std::make_unique<Impl>()), deviceless_(true)
{
    reserveBuffers();
    lastError_ = "No audio device: this engine renders offline";
}

void AudioSystem::reserveBuffers()
{
    // Room for everything the audio thread touches, so it never allocates
    active_.reserve(kMaxProcessors);
    parallel_.reserve(kMaxProcessors);
    for (auto &channel : scratch_)
        channel.assign(kMaxBlock, 0.0f);
    for (auto &channel : inputScratch_)
        channel.assign(kMaxBlock, 0.0f);
}

AudioSystem::~AudioSystem()
{
    stopDevice();
    if (impl_->contextReady)
        ma_context_uninit(&impl_->context);
    // No audio thread now, so the processors can go
    active_.clear();
    parallel_.clear();
    owned_.clear();
}

void AudioSystem::refreshDevices()
{
    devices_.clear();
    inputDevices_.clear();
    impl_->ids.clear();
    impl_->inputIds.clear();
    if (!impl_->contextReady)
        return;
    ma_device_info *playback = nullptr;
    ma_device_info *capture = nullptr;
    ma_uint32 count = 0;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&impl_->context, &playback, &count, &capture, &captureCount) != MA_SUCCESS)
        return;
    for (ma_uint32 i = 0; i < count; ++i) {
        devices_.push_back({playback[i].name, playback[i].isDefault != 0});
        impl_->ids.push_back(playback[i].id);
    }
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        inputDevices_.push_back({capture[i].name, capture[i].isDefault != 0});
        impl_->inputIds.push_back(capture[i].id);
    }
}

void AudioSystem::configure(const AudioSettings &settings)
{
    if (running_)
        restart(settings);
    else
        settings_ = settings;
}

void AudioSystem::adopt(std::unique_ptr<AudioProcessor> processor)
{
    processor->prepare(impl_->processorsRate > 0.0 ? impl_->processorsRate : 48000.0, kMaxBlock);
    AudioProcessor *raw = processor.get();
    owned_.push_back(std::move(processor));
    // The queue only fills if the audio thread is stalled; wait for it rather than drop
    while (!commands_.push({Command::Type::Add, raw}))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!running_)
        applyCommandsHere();
}

void AudioSystem::remove(AudioProcessor &processor)
{
    while (!commands_.push({Command::Type::Remove, &processor}))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!running_) {
        applyCommandsHere();
        deleteRetired();
    }
}

void AudioSystem::applyCommandsHere()
{
    // Only while no audio thread is consuming the queue: this thread plays both sides
    applyCommands();
}

void AudioSystem::deleteRetired()
{
    AudioProcessor *processor = nullptr;
    while (retired_.pop(processor)) {
        owned_.erase(std::remove_if(owned_.begin(), owned_.end(),
                                    [processor](const std::unique_ptr<AudioProcessor> &p) { return p.get() == processor; }),
                     owned_.end());
    }
}

void AudioSystem::prepareOffline(double sampleRate)
{
    if (!running() && impl_->processorsRate != sampleRate)
        prepareAll(sampleRate);
}

void AudioSystem::prepareAll(double sampleRate)
{
    for (auto &processor : owned_)
        processor->prepare(sampleRate, kMaxBlock);
    masterEffects_.prepare(sampleRate, kMaxBlock);
    impl_->processorsRate = sampleRate;
}

bool AudioSystem::openDevice()
{
    if (!impl_->contextReady)
        return false;

    const bool wantInput = settings_.input != AudioSettings::kNoInput;
    ma_device_config config = ma_device_config_init(wantInput ? ma_device_type_duplex : ma_device_type_playback);
    config.playback.format = ma_format_f32;
    if (wantInput) {
        config.capture.format = ma_format_f32;
        config.capture.channels = 0; // the device's own count
        config.capture.shareMode = ma_share_mode_shared;
        if (settings_.input >= 0 && settings_.input < static_cast<int>(impl_->inputIds.size()))
            config.capture.pDeviceID = &impl_->inputIds[static_cast<size_t>(settings_.input)];
    }
    config.playback.channels = kChannels;
    if (settings_.device >= 0 && settings_.device < static_cast<int>(impl_->ids.size()))
        config.playback.pDeviceID = &impl_->ids[static_cast<size_t>(settings_.device)];
    config.sampleRate = settings_.sampleRate;
    config.periodSizeInFrames = settings_.bufferFrames;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.noPreSilencedOutputBuffer = MA_TRUE; // render() writes every sample
    config.pUserData = this;
    config.dataCallback = [](ma_device *device, void *output, const void *input, ma_uint32 frames) {
        AudioSystem::callback(device, output, input, frames);
    };
    config.notificationCallback = [](const ma_device_notification *notification) {
        // May arrive on any thread: only flag it; tick() deals with it on the main thread
        auto *self = static_cast<AudioSystem *>(notification->pDevice->pUserData);
        if (notification->type == ma_device_notification_type_stopped && !self->impl_->stopping.load())
            self->stoppedUnexpectedly_.store(true);
    };

    config.playback.shareMode = settings_.exclusive ? ma_share_mode_exclusive : ma_share_mode_shared;
    ma_result result = ma_device_init(&impl_->context, &config, &impl_->device);
    std::string note;
    if (result != MA_SUCCESS && settings_.exclusive) {
        // Many devices refuse exclusive mode, or not at this rate and size: fall back to shared
        note = std::string("Exclusive mode unavailable (") + ma_result_description(result) + "); using shared mode";
        config.playback.shareMode = ma_share_mode_shared;
        result = ma_device_init(&impl_->context, &config, &impl_->device);
    }
    if (result != MA_SUCCESS && wantInput) {
        // Output without the input is better than nothing
        note = std::string("Couldn't open the input (") + ma_result_description(result) + "); playing without it";
        config.deviceType = ma_device_type_playback;
        result = ma_device_init(&impl_->context, &config, &impl_->device);
    }
    if (result != MA_SUCCESS) {
        lastError_ = std::string("Couldn't open the audio device: ") + ma_result_description(result);
        return false;
    }
    exclusive_ = config.playback.shareMode == ma_share_mode_exclusive;
    const bool duplex = config.deviceType == ma_device_type_duplex;
    inputChannels_ = duplex ? std::min<uint32_t>(impl_->device.capture.channels, kMaxInputs) : 0;
    inputName_ = duplex ? impl_->device.capture.name : "";
    roundTripFrames_ = impl_->device.playback.internalPeriodSizeInFrames +
                       (duplex ? impl_->device.capture.internalPeriodSizeInFrames : 0);
    impl_->deviceReady = true;

    // Nothing is processing yet, so processors can be prepared and queued changes applied here
    const uint32_t rate = impl_->device.sampleRate;
    prepareAll(rate);
    applyCommandsHere();
    deleteRetired();

    result = ma_device_start(&impl_->device);
    if (result != MA_SUCCESS) {
        lastError_ = std::string("Couldn't start the audio device: ") + ma_result_description(result);
        ma_device_uninit(&impl_->device);
        impl_->deviceReady = false;
        return false;
    }

    running_ = true;
    lastError_ = note;
    sampleRate_ = rate;
    bufferFrames_ = impl_->device.playback.internalPeriodSizeInFrames;
    deviceName_ = impl_->device.playback.name;
    env_.events().publish(AudioDeviceStarted{deviceName_, sampleRate_, bufferFrames_});
    return true;
}

void AudioSystem::stopDevice()
{
    if (!impl_->deviceReady)
        return;
    impl_->stopping.store(true);
    ma_device_uninit(&impl_->device); // stops it first; returns once the callback has finished
    impl_->stopping.store(false);
    impl_->deviceReady = false;
    const bool wasRunning = running_;
    running_ = false;
    inputChannels_ = 0;
    inputName_.clear();
    // The audio thread is gone: finish its pending work here
    applyCommandsHere();
    deleteRetired();
    if (wasRunning)
        env_.events().publish(AudioDeviceStopped{});
}

bool AudioSystem::restart(const AudioSettings &settings)
{
    stopDevice();
    settings_ = settings;
    return openDevice();
}

float AudioSystem::takePeak(uint32_t channel)
{
    return channel < kChannels ? peaks_[channel].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
}

float AudioSystem::takeInputPeak(uint32_t channel)
{
    return channel < kMaxInputs ? inputPeaks_[channel].exchange(0.0f, std::memory_order_relaxed) : 0.0f;
}

bool AudioSystem::renderOffline(float *out, uint32_t frames, double sampleRate, const float *input,
                                uint32_t inputChannels)
{
    if (running_)
        return false;
    if (impl_->processorsRate != sampleRate)
        prepareAll(sampleRate);
    render(out, frames, sampleRate, input, input ? inputChannels : 0);
    deleteRetired();
    return true;
}

void AudioSystem::start(env::Environment &)
{
    if (!deviceless_)
        openDevice();
}

void AudioSystem::collectOffline()
{
    // Processors the audio thread has let go of
    deleteRetired();
}

void AudioSystem::tick(env::Environment &)
{
    // Processors the audio thread has let go of
    collectOffline();

    // The device stopped on its own (unplugged, or taken by another program)
    if (stoppedUnexpectedly_.exchange(false) && running_) {
        impl_->stopping.store(true);
        ma_device_uninit(&impl_->device);
        impl_->stopping.store(false);
        impl_->deviceReady = false;
        running_ = false;
        inputChannels_ = 0;
        inputName_.clear();
        lastError_ = "The audio device stopped";
        applyCommandsHere();
        deleteRetired();
        env_.events().publish(AudioDeviceStopped{lastError_});
    }
}

void AudioSystem::stop(env::Environment &)
{
    stopDevice();
}

// Audio thread

void AudioSystem::callback(void *device, void *output, const void *input, uint32_t frames)
{
    auto *d = static_cast<ma_device *>(device);
    auto *self = static_cast<AudioSystem *>(d->pUserData);
    disableDenormals();
    // The input's interleaved stride is the device's whole channel count, which may exceed ours
    const uint32_t stride = input ? d->capture.channels : 0;
    self->render(static_cast<float *>(output), frames, d->sampleRate, static_cast<const float *>(input), stride);
}

void AudioSystem::applyCommands()
{
    Command command;
    bool changed = false;
    while (commands_.pop(command)) {
        if (command.type == Command::Type::Add) {
            // Full graph: drop it (it stays owned, and is deleted with the system)
            if (active_.size() < active_.capacity())
                active_.push_back(command.processor);
        } else {
            auto it = std::find(active_.begin(), active_.end(), command.processor);
            if (it != active_.end())
                active_.erase(it); // shifts pointers; never allocates
            // Back to the main thread to be deleted. retired_ is as big as commands_, so it
            // can't fill up faster than commands arrive.
            retired_.push(command.processor);
        }
        changed = true;
    }
    // The processors that render in parallel, in graph order (reserved: never allocates)
    if (changed) {
        parallel_.clear();
        for (AudioProcessor *p : active_) {
            if (p->parallel())
                parallel_.push_back(p);
        }
    }
}

void AudioSystem::render(float *out, uint32_t frames, double sampleRate, const float *input, uint32_t inputStride)
{
    const uint32_t inputs = std::min(inputStride, kMaxInputs);
    float inputPeak[kMaxInputs] = {};
    using Clock = std::chrono::steady_clock;
    const auto begin = Clock::now();

    applyCommands();
    if (sampleRate != preparedRate_) {
        gain_.prepare(sampleRate);
        gain_.reset(masterGain_.get());
        preparedRate_ = sampleRate;
    }

    float peak[kChannels] = {};
    uint32_t done = 0;
    while (done < frames) {
        // A block ends at the loop's end, so the next starts back at its start on the sample
        const TransportState state = transport_.begin(sampleRate);
        const uint32_t n = std::max(1u, transport_.framesBeforeLoop(state, std::min(kMaxBlock, frames - done)));
        for (auto &channel : scratch_)
            std::fill_n(channel.data(), n, 0.0f);

        // The input, planar, for processors to read
        const float *inputChannels[kMaxInputs] = {};
        for (uint32_t c = 0; c < inputs; ++c) {
            float *dest = inputScratch_[c].data();
            const float *src = input + static_cast<size_t>(done) * inputStride + c;
            float p = inputPeak[c];
            for (uint32_t i = 0; i < n; ++i) {
                dest[i] = src[static_cast<size_t>(i) * inputStride];
                p = std::max(p, std::fabs(dest[i]));
            }
            inputPeak[c] = p;
            inputChannels[c] = dest;
        }

        // Every processor adds into the master bus
        float *channels[kChannels];
        for (uint32_t c = 0; c < kChannels; ++c)
            channels[c] = scratch_[c].data();
        AudioBlock block{channels, kChannels, n, sampleRate, state, inputs ? inputChannels : nullptr, inputs};
        // Everything else in order (the MIDI hub before the tracks that read it), then the tracks
        // at once across the pool, added into the mix in order
        for (AudioProcessor *processor : active_) {
            if (!processor->parallel())
                processor->process(block);
        }
        if (!parallel_.empty()) {
            ParallelBlock job{parallel_.data(), &block};
            if (pool_)
                pool_->run(static_cast<uint32_t>(parallel_.size()), &AudioSystem::renderJob, &job);
            else
                for (uint32_t p = 0; p < parallel_.size(); ++p)
                    renderJob(&job, p);
            for (AudioProcessor *processor : parallel_)
                processor->mixInto(block);
        }

        masterEffects_.process(scratch_[0].data(), scratch_[1].data(), n, &state);

        // Master gain, metering, and interleaving into the device's buffer
        const float target = masterGain_.get();
        float *o = out + static_cast<size_t>(done) * kChannels;
        for (uint32_t i = 0; i < n; ++i) {
            const float g = gain_.next(target);
            for (uint32_t c = 0; c < kChannels; ++c) {
                const float v = scratch_[c][i] * g;
                o[i * kChannels + c] = v;
                peak[c] = std::max(peak[c], std::fabs(v));
            }
        }
        transport_.advance(state, n);
        done += n;
    }

    // Peaks only grow until the main thread takes them
    auto raise = [](std::atomic<float> &slot, float value) {
        float previous = slot.load(std::memory_order_relaxed);
        while (value > previous && !slot.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
        }
    };
    for (uint32_t c = 0; c < kChannels; ++c)
        raise(peaks_[c], peak[c]);
    for (uint32_t c = 0; c < inputs; ++c)
        raise(inputPeaks_[c], inputPeak[c]);

    // Load: processing time against the time the block lasts
    const double elapsed = std::chrono::duration<double>(Clock::now() - begin).count();
    const double budget = frames / sampleRate;
    const float ratio = budget > 0.0 ? static_cast<float>(elapsed / budget) : 0.0f;
    const float smoothed = load_.load(std::memory_order_relaxed);
    load_.store(smoothed + (ratio - smoothed) * 0.1f, std::memory_order_relaxed);
    if (ratio > 1.0f)
        overloads_.fetch_add(1, std::memory_order_relaxed);
    callbacks_.fetch_add(1, std::memory_order_relaxed);
}

}
