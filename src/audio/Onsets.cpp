#include "Onsets.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "Fft.h"

namespace audio {

namespace {

constexpr double kMinGapSeconds = 0.040;
constexpr int kMedianFrames = 12; // either side of a frame, for its threshold

// The novelty's autocorrelation at a fractional lag (linear between whole lags)
double correlationAt(const std::vector<double> &r, double lag)
{
    if (lag < 0.0 || lag + 1.0 >= static_cast<double>(r.size()))
        return 0.0;
    const auto i = static_cast<size_t>(lag);
    const double f = lag - static_cast<double>(i);
    return r[i] + (r[i + 1] - r[i]) * f;
}

}

OnsetAnalysis analyseOnsets(const float *interleaved, uint64_t frames, uint32_t channels, double sampleRate,
                            float sensitivity)
{
    OnsetAnalysis out;
    if (!interleaved || frames == 0 || channels == 0 || sampleRate <= 0.0)
        return out;
    const size_t n = sampleRate > 64000.0 ? 2048 : 1024;
    const size_t hop = n / 4;
    out.hopSeconds = static_cast<double>(hop) / sampleRate;

    // Mono
    std::vector<float> mono(static_cast<size_t>(frames));
    float loudest = 0.0f;
    for (uint64_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (uint32_t c = 0; c < channels; ++c)
            sum += interleaved[i * channels + c];
        mono[static_cast<size_t>(i)] = sum / static_cast<float>(channels);
        loudest = std::max(loudest, std::fabs(mono[static_cast<size_t>(i)]));
    }
    if (loudest < 0.001f)
        return out;

    // The flux: how much each bin's log magnitude rose since the frame before, summed. Frame j is
    // centred on sample j * hop.
    RealFft fft(n);
    const std::vector<float> window = hannWindow(n);
    const size_t bins = fft.bins();
    std::vector<float> frame(n), re(bins), im(bins), previous(bins, 0.0f), level(bins);
    const size_t count = static_cast<size_t>(frames / hop) + 1;
    out.novelty.assign(count, 0.0f);
    std::vector<float> energy(count, 0.0f);
    for (size_t j = 0; j < count; ++j) {
        const int64_t first = static_cast<int64_t>(j * hop) - static_cast<int64_t>(n / 2);
        float e = 0.0f;
        for (size_t k = 0; k < n; ++k) {
            const int64_t at = first + static_cast<int64_t>(k);
            const float x = at >= 0 && at < static_cast<int64_t>(frames) ? mono[static_cast<size_t>(at)] : 0.0f;
            frame[k] = x * window[k];
            e = std::max(e, std::fabs(x));
        }
        energy[j] = e;
        fft.forward(frame.data(), re.data(), im.data());
        float flux = 0.0f;
        for (size_t k = 1; k < bins; ++k) {
            const float magnitude = std::sqrt(re[k] * re[k] + im[k] * im[k]) / static_cast<float>(n);
            level[k] = std::log1p(1000.0f * magnitude);
            flux += std::max(0.0f, level[k] - previous[k]);
        }
        std::swap(previous, level);
        out.novelty[j] = j == 0 ? 0.0f : flux;
    }

    // Peaks over the local median plus a share of the mean
    double mean = 0.0;
    for (float v : out.novelty)
        mean += v;
    mean /= static_cast<double>(count);
    const double share = 0.25 + (1.0 - std::clamp(static_cast<double>(sensitivity), 0.0, 1.0)) * 1.5;
    std::vector<float> local;
    std::vector<size_t> peaks;
    for (size_t j = 1; j + 1 < count; ++j) {
        const float v = out.novelty[j];
        if (v < out.novelty[j - 1] || v < out.novelty[j + 1] || energy[j] < 0.001f)
            continue;
        const size_t from = j > static_cast<size_t>(kMedianFrames) ? j - kMedianFrames : 0;
        const size_t to = std::min(count, j + kMedianFrames + 1);
        local.assign(out.novelty.begin() + static_cast<std::ptrdiff_t>(from), out.novelty.begin() + static_cast<std::ptrdiff_t>(to));
        std::nth_element(local.begin(), local.begin() + static_cast<std::ptrdiff_t>(local.size() / 2), local.end());
        const double threshold = local[local.size() / 2] + share * mean;
        if (v > threshold)
            peaks.push_back(j);
    }

    // Each peak moved to where its rise begins: in the frame's window, the first sample where the
    // envelope's rise over a millisecond reaches half its largest
    const auto ms = std::max<size_t>(1, static_cast<size_t>(sampleRate / 1000.0));
    std::vector<float> envelope(mono.size());
    {
        const float attack = 1.0f - std::exp(-1.0f / static_cast<float>(ms) * 4.0f);
        const float release = 1.0f - std::exp(-1.0f / (static_cast<float>(ms) * 10.0f));
        float env = 0.0f;
        for (size_t i = 0; i < mono.size(); ++i) {
            const float x = std::fabs(mono[i]);
            env += (x - env) * (x > env ? attack : release);
            envelope[i] = env;
        }
    }
    std::vector<double> found;
    for (size_t j : peaks) {
        const int64_t centre = static_cast<int64_t>(j * hop);
        const auto from = static_cast<size_t>(std::max<int64_t>(static_cast<int64_t>(ms), centre - static_cast<int64_t>(n / 2)));
        const auto to = static_cast<size_t>(std::min<int64_t>(static_cast<int64_t>(mono.size()), centre + static_cast<int64_t>(n / 2)));
        if (from >= to)
            continue;
        float best = 0.0f;
        for (size_t i = from; i < to; ++i)
            best = std::max(best, envelope[i] - envelope[i - ms]);
        size_t at = from;
        for (size_t i = from; i < to; ++i) {
            if (envelope[i] - envelope[i - ms] >= best * 0.5f) {
                at = i > ms ? i - ms : 0;
                break;
            }
        }
        found.push_back(static_cast<double>(at) / sampleRate);
    }
    std::sort(found.begin(), found.end());
    for (double t : found) {
        if (out.transients.empty() || t - out.transients.back() >= kMinGapSeconds)
            out.transients.push_back(t);
    }
    return out;
}

double estimateTempo(const OnsetAnalysis &onsets)
{
    const std::vector<float> &nov = onsets.novelty;
    if (nov.size() < 16 || onsets.hopSeconds <= 0.0)
        return 0.0;
    double mean = 0.0;
    for (float v : nov)
        mean += v;
    mean /= static_cast<double>(nov.size());
    // Lags up to two beats at 60 BPM
    const auto maxLag = std::min(nov.size() - 1, static_cast<size_t>(2.0 / onsets.hopSeconds) + 2);
    std::vector<double> r(maxLag + 1, 0.0);
    for (size_t lag = 0; lag <= maxLag; ++lag) {
        double sum = 0.0;
        for (size_t i = 0; i + lag < nov.size(); ++i)
            sum += (nov[i] - mean) * (nov[i + lag] - mean);
        r[lag] = sum / static_cast<double>(nov.size() - lag);
    }
    double bestBpm = 0.0, bestScore = 0.0;
    for (double bpm = 60.0; bpm <= 200.0; bpm += 0.05) {
        const double lag = 60.0 / bpm / onsets.hopSeconds;
        const double weight = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 0.9, 2.0));
        const double score = (correlationAt(r, lag) + 0.5 * correlationAt(r, lag * 2.0)) * weight;
        if (score > bestScore) {
            bestScore = score;
            bestBpm = bpm;
        }
    }
    if (bestBpm <= 0.0)
        return 0.0;
    while (bestBpm < 80.0)
        bestBpm *= 2.0;
    while (bestBpm > 160.0)
        bestBpm /= 2.0;
    return bestBpm;
}

double tempoFromLength(double seconds, double estimate)
{
    if (seconds <= 0.0 || estimate <= 0.0)
        return 0.0;
    double best = 0.0, bestError = 0.03;
    for (int bars : {1, 2, 4, 8, 16}) {
        const double bpm = bars * 4.0 * 60.0 / seconds;
        if (bpm < 40.0 || bpm > 250.0)
            continue;
        // The estimate itself first: twice or half it only when nothing is near it
        for (double e : {estimate, estimate * 2.0, estimate / 2.0}) {
            const double error = std::fabs(bpm / e - 1.0) + (e == estimate ? 0.0 : 0.01);
            if (error < bestError) {
                bestError = error;
                best = bpm;
            }
        }
    }
    return best;
}

double bpmFromName(const std::string &name, bool *named)
{
    std::string s;
    for (char c : name)
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // A number before "bpm", possibly with a space, underscore or dash between
    for (size_t at = s.find("bpm"); at != std::string::npos; at = s.find("bpm", at + 1)) {
        size_t end = at;
        while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '_' || s[end - 1] == '-'))
            --end;
        size_t begin = end;
        while (begin > 0 && (std::isdigit(static_cast<unsigned char>(s[begin - 1])) || s[begin - 1] == '.'))
            --begin;
        if (begin < end) {
            const double bpm = std::atof(s.substr(begin, end - begin).c_str());
            if (bpm >= 40.0 && bpm <= 300.0) {
                if (named)
                    *named = true;
                return bpm;
            }
        }
    }
    // A whole number 60-200 standing alone between separators
    auto separator = [](char c) { return c == '_' || c == ' ' || c == '-' || c == '.' || c == '(' || c == ')' || c == '[' || c == ']'; };
    for (size_t i = 0; i < s.size();) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
            ++j;
        const bool before = i == 0 || separator(s[i - 1]);
        const bool after = j == s.size() || separator(s[j]);
        if (before && after && (j - i == 2 || j - i == 3)) {
            const int bpm = std::atoi(s.substr(i, j - i).c_str());
            if (bpm >= 60 && bpm <= 200) {
                if (named)
                    *named = false;
                return bpm;
            }
        }
        i = j;
    }
    return 0.0;
}

}
