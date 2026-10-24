#include "audio/AudioAnalysis.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstdio>
#include <cstdlib>

#include <miniaudio.h>

#include "audio/AudioFile.h"

namespace audio {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kMinBpm = 60.0;
constexpr double kMaxBpm = 200.0;
// Onsets closer than this are one
constexpr double kMinGapSeconds = 0.04;
// The tempo is looked for in at most this much of a file
constexpr double kTempoSeconds = 120.0;

// The flux at a fractional hop, between its neighbours
float fluxAt(const std::vector<float> &v, double i)
{
    if (i < 0.0)
        return 0.0f;
    const auto k = static_cast<size_t>(i);
    if (k + 1 >= v.size())
        return k < v.size() ? v[k] : 0.0f;
    const auto f = static_cast<float>(i - static_cast<double>(k));
    return v[k] + (v[k + 1] - v[k]) * f;
}

}

OnsetAnalyser::OnsetAnalyser(double sampleRate)
    : rate_(sampleRate),
      hop_(256 * static_cast<size_t>(std::max(1.0, std::round(sampleRate / 48000.0)))),
      size_(hop_ * 4),
      fft_(size_)
{
    window_.assign(size_, 0.0f);
    hann_.resize(size_);
    for (size_t i = 0; i < size_; ++i)
        hann_[i] = static_cast<float>(0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(size_)));
    buffer_.resize(size_);
    re_.resize(fft_.bins());
    im_.resize(fft_.bins());
    previous_.assign(fft_.bins(), 0.0f);
}

void OnsetAnalyser::add(const float *interleaved, size_t frames, uint32_t channels)
{
    const uint32_t ch = std::max<uint32_t>(1, channels);
    for (size_t f = 0; f < frames; ++f) {
        float mono = 0.0f;
        for (uint32_t c = 0; c < ch; ++c)
            mono += interleaved[f * ch + c];
        mono /= static_cast<float>(ch);
        // The window slides by a hop: the newest samples at its end
        window_[size_ - hop_ + filled_] = mono;
        energyRun_ += static_cast<double>(mono) * mono;
        if (++energyCount_ == kEnergyFrames) {
            energy_.push_back(static_cast<float>(energyRun_ / kEnergyFrames));
            energyRun_ = 0.0;
            energyCount_ = 0;
        }
        ++frames_;
        if (++filled_ == hop_) {
            frame();
            std::copy(window_.begin() + static_cast<std::ptrdiff_t>(hop_), window_.end(), window_.begin());
            filled_ = 0;
        }
    }
}

void OnsetAnalyser::frame()
{
    for (size_t i = 0; i < size_; ++i)
        buffer_[i] = window_[i] * hann_[i];
    fft_.forward(buffer_.data(), re_.data(), im_.data());
    // The rise of each bin's log magnitude, up to about 16 kHz
    const size_t top = std::min(fft_.bins(), static_cast<size_t>(16000.0 / (rate_ / static_cast<double>(size_))) + 1);
    float flux = 0.0f;
    for (size_t k = 1; k < top; ++k) {
        const float mag = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
        const float level = std::log1p(1000.0f * mag);
        const float rise = level - previous_[k];
        if (rise > 0.0f)
            flux += rise;
        previous_[k] = level;
    }
    flux_.push_back(flux);
}

AudioAnalysis OnsetAnalyser::finish()
{
    AudioAnalysis a;
    a.sampleRate = rate_;
    a.frames = frames_;
    if (energyCount_ > 0)
        energy_.push_back(static_cast<float>(energyRun_ / energyCount_));
    if (flux_.size() < 4)
        return a;

    // Normalised to the strongest (the 99.5th percentile, so one spike doesn't flatten the rest)
    std::vector<float> sorted = flux_;
    const size_t at = std::min(sorted.size() - 1, static_cast<size_t>(static_cast<double>(sorted.size()) * 0.995));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(at), sorted.end());
    const float top = std::max(sorted[at], 1e-6f);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const float median = sorted[sorted.size() / 2];
    std::vector<float> odf(flux_.size());
    for (size_t i = 0; i < flux_.size(); ++i)
        odf[i] = std::min(1.0f, flux_[i] / top);

    // Peaks: the largest within 3 hops, above the mean of the 100 ms before and 30 ms after plus a floor
    const double hopsPerSecond = rate_ / static_cast<double>(hop_);
    const auto before = static_cast<size_t>(std::max(1.0, 0.1 * hopsPerSecond));
    const auto after = static_cast<size_t>(std::max(1.0, 0.03 * hopsPerSecond));
    const auto gap = static_cast<size_t>(std::max(1.0, kMinGapSeconds * hopsPerSecond));
    std::vector<double> prefix(odf.size() + 1, 0.0);
    for (size_t i = 0; i < odf.size(); ++i)
        prefix[i + 1] = prefix[i] + odf[i];
    size_t last = 0;
    bool any = false;
    double onsetMass = 0.0;
    for (size_t i = 0; i < odf.size(); ++i) {
        const float v = odf[i];
        // Held sound's flux wanders a few times its median; an onset stands well clear of it
        if (v < 0.08f || flux_[i] < 5.0f * median + 20.0f)
            continue;
        bool peak = true;
        for (size_t j = i >= 3 ? i - 3 : 0; j <= std::min(odf.size() - 1, i + 3) && peak; ++j)
            peak = j == i || odf[j] < v || (odf[j] == v && j > i);
        if (!peak)
            continue;
        const size_t lo = i >= before ? i - before : 0;
        const size_t hi = std::min(odf.size(), i + after + 1);
        const double mean = (prefix[hi] - prefix[lo]) / static_cast<double>(hi - lo);
        if (v < mean + 0.06)
            continue;
        if (any && i - last < gap)
            continue;
        // Frame i spans the `size_` frames up to (i + 1) hops: the onset is somewhere in it (the log
        // magnitudes rise as soon as it enters), so it is placed on the energy's steepest rise there,
        // in runs of kEnergyFrames
        const double end = static_cast<double>((i + 1) * hop_);
        const double estimate = end - static_cast<double>(size_) * 0.5;
        const auto from = static_cast<int64_t>(std::floor((end - static_cast<double>(size_)) / kEnergyFrames));
        const auto to = static_cast<int64_t>((end + static_cast<double>(hop_)) / kEnergyFrames);
        int64_t best = -1;
        double bestRise = 0.0;
        for (int64_t b = std::max<int64_t>(0, from); b <= to && b < static_cast<int64_t>(energy_.size()); ++b) {
            // Before the file is silence
            const double before = b > 0 ? energy_[static_cast<size_t>(b - 1)] : 0.0;
            const double rise = std::log(energy_[static_cast<size_t>(b)] + 1e-9) - std::log(before + 1e-9);
            if (best < 0 || rise > bestRise) {
                best = b;
                bestRise = rise;
            }
        }
        uint64_t frame = best >= 0 ? static_cast<uint64_t>(best) * kEnergyFrames
                                   : static_cast<uint64_t>(std::max(0.0, estimate));
        frame = std::min(frame, frames_ > 0 ? frames_ - 1 : 0);
        if (!a.transients.empty() && frame <= a.transients.back().frame)
            continue;
        a.transients.push_back({frame, v});
        onsetMass += prefix[std::min(odf.size(), i + 3)] - prefix[i >= 2 ? i - 2 : 0];
        last = i;
        any = true;
    }
    // How much of the flux is in its peaks: hits make it high, held sound low
    const double total = prefix.back();
    a.percussive = total > 0.0 ? static_cast<float>(std::clamp(onsetMass / total, 0.0, 1.0)) : 0.0f;

    // The tempo, from the first couple of minutes
    std::vector<float> head(odf.begin(), odf.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                         odf.size(), static_cast<size_t>(kTempoSeconds * hopsPerSecond))));
    a.bpm = tempoOfFlux(head, hopsPerSecond, &a.confidence);

    // A loop: a whole number of 4/4 bars at a tempo near the one found (or its half or double)
    const double seconds = a.seconds();
    if (a.bpm > 0.0 && seconds > 0.5) {
        double bestMiss = 0.03;
        for (int bars : {1, 2, 4, 8, 16, 32}) {
            const double tempo = bars * 4.0 * 60.0 / seconds;
            if (tempo < 70.0 || tempo > 180.0)
                continue;
            for (double f : {1.0, 0.5, 2.0}) {
                const double miss = std::fabs(tempo / (a.bpm * f) - 1.0);
                if (miss < bestMiss) {
                    bestMiss = miss;
                    a.loop = true;
                    a.loopBars = bars;
                    a.bpm = tempo;
                }
            }
        }
    }
    // The first beat: the first strong transient near the start (a loop's must be right at it)
    if (a.bpm > 0.0) {
        const double within = a.loop ? 0.03 : 0.5 * 60.0 / a.bpm;
        for (const Transient &t : a.transients) {
            const double s = static_cast<double>(t.frame) / rate_;
            if (s > within)
                break;
            if (t.strength >= 0.3f) {
                a.downbeat = s;
                break;
            }
        }
    }
    return a;
}

double tempoOfFlux(const std::vector<float> &flux, double hopsPerSecond, float *confidence)
{
    if (confidence)
        *confidence = 0.0f;
    const auto maxLag = static_cast<size_t>(std::ceil(hopsPerSecond * 60.0 / kMinBpm * 4.0)) + 2;
    if (flux.size() < static_cast<size_t>(hopsPerSecond * 60.0 / kMinBpm) * 3)
        return 0.0;
    // The flux less its mean, autocorrelated at every lag up to four beats at the slowest tempo
    const double mean = std::accumulate(flux.begin(), flux.end(), 0.0) / static_cast<double>(flux.size());
    std::vector<float> centred(flux.size());
    for (size_t i = 0; i < flux.size(); ++i)
        centred[i] = static_cast<float>(flux[i] - mean);
    const size_t lags = std::min(maxLag, flux.size() / 2);
    std::vector<float> ac(lags, 0.0f);
    for (size_t lag = 0; lag < lags; ++lag) {
        double sum = 0.0;
        for (size_t i = lag; i < centred.size(); ++i)
            sum += static_cast<double>(centred[i]) * centred[i - lag];
        ac[lag] = static_cast<float>(sum / static_cast<double>(centred.size() - lag));
    }
    if (ac[0] <= 0.0f)
        return 0.0;
    // Each tempo scored by its beat and the beats at twice and four times its period, weighted toward
    // 120 on a log scale; tenths of a BPM
    double best = 0.0, bestScore = -1e30, sumScore = 0.0;
    int count = 0;
    for (double bpm = kMinBpm; bpm <= kMaxBpm + 1e-9; bpm += 0.1) {
        const double lag = hopsPerSecond * 60.0 / bpm;
        const double score = fluxAt(ac, lag) + 0.5 * fluxAt(ac, lag * 2.0) + 0.25 * fluxAt(ac, lag * 4.0) +
                             0.25 * fluxAt(ac, lag * 0.5);
        const double octave = std::log2(bpm / 120.0);
        const double weighted = score * std::exp(-0.5 * octave * octave / (0.9 * 0.9));
        sumScore += weighted;
        ++count;
        if (weighted > bestScore) {
            bestScore = weighted;
            best = bpm;
        }
    }
    if (bestScore <= 0.0)
        return 0.0;
    if (confidence) {
        const double average = sumScore / std::max(1, count);
        *confidence = static_cast<float>(std::clamp((bestScore - average) / (static_cast<double>(ac[0]) * 1.75), 0.0, 1.0));
    }
    return std::round(best * 10.0) / 10.0;
}

AudioAnalysis analyseSamples(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate)
{
    OnsetAnalyser analyser(sampleRate);
    constexpr uint64_t kPiece = 16384;
    const uint32_t ch = std::max<uint32_t>(1, channels);
    for (uint64_t at = 0; at < frames; at += kPiece)
        analyser.add(interleaved + at * ch, static_cast<size_t>(std::min(kPiece, frames - at)), ch);
    return analyser.finish();
}

std::shared_ptr<AudioAnalysis> analyseAudioFile(const std::string &path, std::string &error)
{
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) {
        error = "Can't open " + path + " as audio";
        return nullptr;
    }
    ma_format format;
    ma_uint32 channels = 0;
    ma_uint32 rate = 0;
    ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0);
    if (channels == 0 || rate == 0) {
        ma_decoder_uninit(&decoder);
        error = "Unknown format in " + path;
        return nullptr;
    }
    OnsetAnalyser analyser(rate);
    std::vector<float> piece(static_cast<size_t>(16384) * channels);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder, piece.data(), 16384, &read);
        analyser.add(piece.data(), static_cast<size_t>(read), channels);
        if (result != MA_SUCCESS || read < 16384)
            break;
    }
    ma_decoder_uninit(&decoder);
    return std::make_shared<AudioAnalysis>(analyser.finish());
}

std::shared_ptr<AudioAnalysis> analyseAudio(const AudioData &data, std::string &error)
{
    if (!data.streamed() && !data.samples.empty())
        return std::make_shared<AudioAnalysis>(analyseSamples(data.samples.data(), data.frames, data.channels, data.sampleRate));
    return analyseAudioFile(data.path, error);
}

}
