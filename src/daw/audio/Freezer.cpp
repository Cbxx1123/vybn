#include "daw/audio/Freezer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <utility>

#include "daw/model/Freeze.h"
#include "audio/Encoders.h"
#include "env/Environment.h"

namespace daw {

namespace fs = std::filesystem;

namespace {

constexpr double kTailSeconds = 10.0; // a render rings on until quiet, this long at most

// What a render shares with the background job that runs it
struct Run {
    std::unique_ptr<RenderPass> pass;
    std::atomic<bool> done{false};
    bool ok = false;
    bool cancelled = false;
    std::string error;
    uint64_t frames = 0; // written to the file
};

// A name Windows takes for a file
std::string fileSafe(std::string name)
{
    for (char &c : name)
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    return name.empty() ? std::string("Track") : name;
}

// The render's float file as a 32-bit float WAV, its first `skip` frames left out
bool writeWav(const std::string &temp, const std::string &path, uint64_t skip, uint64_t &frames, std::string &error)
{
    audio::FloatTempReader in;
    if (!in.open(temp, error))
        return false;
    audio::EncodeFormat format;
    format.container = audio::EncodeFormat::Wav;
    format.bits = 32;
    format.sampleRate = static_cast<uint32_t>(in.sampleRate());
    format.channels = in.channels();
    std::unique_ptr<audio::AudioWriter> out = audio::makeWriter(format);
    if (!out || !out->open(path, format, {}, error))
        return false;
    in.seek(std::min(skip, in.frames()));
    std::vector<float> block(8192 * static_cast<size_t>(in.channels()));
    frames = 0;
    for (;;) {
        const uint64_t got = in.read(block.data(), 8192);
        if (got == 0)
            break;
        if (!out->write(block.data(), got))
            break;
        frames += got;
    }
    return out->close(error);
}

}

struct Freezer::Job {
    ObjectId track = kNoId;
    FreezeKind kind = FreezeKind::Freeze;
    Project snapshot;
    int index = 0; // the track's in the snapshot
    uint64_t hash = 0;
    std::string path, temp;
    uint32_t rate = 48000;
    uint64_t frames = 0;
    uint32_t latency = 0;
    std::string status = "Waiting";
    bool rendering = false;
    bool cancelling = false;
    std::shared_ptr<Run> run;
    std::unique_ptr<OfflineEngine> engine;
    env::JobGroup group; // after the engine: waited for before it goes
};

Freezer::Freezer(env::Environment &env, plugin::PluginManager *plugins, InstrumentLibrary &library, AudioPool &pool)
    : env_(env), plugins_(plugins), library_(library), pool_(pool)
{
}

Freezer::~Freezer()
{
    for (auto &job : jobs_) {
        if (job->run && job->run->pass)
            job->run->pass->cancel();
    }
    jobs_.clear(); // each waits for its render, then frees its engine
}

bool Freezer::start(const Project &project, int index, FreezeKind kind, const std::string &folder, uint32_t sampleRate,
                    std::string &why)
{
    const Track *t = project.track(index);
    if (!t || !canFreeze(*t)) {
        why = "Only audio and instrument tracks can be frozen or bounced";
        return false;
    }
    if (busy(t->id)) {
        why = "\"" + t->name + "\" is already being rendered";
        return false;
    }
    double end = 0.0;
    for (const Clip &c : t->clips)
        end = std::max(end, c.end());
    if (end <= 0.0) {
        why = "\"" + t->name + "\" has nothing to render";
        return false;
    }

    auto job = std::make_unique<Job>();
    job->track = t->id;
    job->kind = kind;
    job->hash = freezeHash(project, index);
    job->rate = sampleRate > 0 ? sampleRate : 48000;
    job->frames = static_cast<uint64_t>(std::ceil(project.timing.secondsAt(end) * job->rate));

    // The track and the tracks it is keyed by, straight to the master, which has nothing on it
    std::vector<ObjectId> keys;
    for (const audio::DeviceSetting &d : t->effects)
        if (d.sidechain != kNoId)
            keys.push_back(d.sidechain);
    Project snapshot = project;
    snapshot.tracks.clear();
    for (size_t i = 0; i < project.tracks.size(); ++i) {
        const Track &from = project.tracks[i];
        const bool target = static_cast<int>(i) == index;
        if (!target && std::find(keys.begin(), keys.end(), from.id) == keys.end())
            continue;
        if (target)
            job->index = static_cast<int>(snapshot.tracks.size());
        Track kept = from;
        kept.output = kNoId;
        kept.sends.clear();
        kept.solo = false;
        if (target)
            kept.freeze = {}; // rendered live, whatever it has now
        snapshot.tracks.push_back(std::move(kept));
    }
    snapshot.masterEffects.clear();
    snapshot.masterAutomation.clear();
    snapshot.masterVolume = 1.0f;
    snapshot.looping = false;
    snapshot.selectedTrack = -1;
    snapshot.selectedClip = -1;
    job->snapshot = std::move(snapshot);

    // "<track> freeze N.wav" in the folder, the first N not taken
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    const std::string base = fileSafe(t->name) + (kind == FreezeKind::Bounce ? " bounce" : " freeze");
    for (int n = 1;; ++n) {
        const fs::path candidate = fs::u8path(folder) / fs::u8path(base + " " + std::to_string(n) + ".wav");
        if (!fs::exists(candidate, ec)) {
            job->path = candidate.u8string();
            break;
        }
    }
    const fs::path temps = fs::temp_directory_path(ec) / "VYBN freeze";
    fs::create_directories(temps, ec);
    job->temp = (temps / fs::u8path(std::to_string(t->id) + "-" + std::to_string(reinterpret_cast<uintptr_t>(job.get())) + ".f32")).u8string();
    jobs_.push_back(std::move(job));
    return true;
}

void Freezer::startRender(Job &job)
{
    RenderPassConfig config;
    config.sampleRate = job.rate;
    config.startBeat = 0.0;
    config.preFader = true;
    config.unmuted = true;
    PassOutput output;
    output.master = false;
    output.tracks = {job.index};
    output.respectMuteSolo = false;
    output.from = 0;
    output.to = job.frames + job.latency;
    output.tail = static_cast<uint64_t>(kTailSeconds * job.rate);
    output.path = job.temp;
    config.outputs = {output};
    job.run = std::make_shared<Run>();
    job.run->pass = std::make_unique<RenderPass>(*job.engine, config);
    job.rendering = true;
    job.status = "Rendering";
    std::shared_ptr<Run> run = job.run;
    const std::string temp = job.temp, path = job.path;
    const uint32_t latency = job.latency;
    env::runJob(env_, env::Lane::Background, [run, temp, path, latency] {
        std::string error;
        run->ok = run->pass->run(error);
        run->cancelled = run->pass->cancelled();
        if (run->ok)
            run->ok = writeWav(temp, path, latency, run->frames, error);
        std::error_code ec;
        fs::remove(fs::u8path(temp), ec);
        run->error = error;
        run->done.store(true);
    }, job.group);
}

void Freezer::finish(Job &job, Result result)
{
    result.track = job.track;
    result.kind = job.kind;
    result.hash = job.hash;
    finished_.push_back(std::move(result));
}

void Freezer::update()
{
    if (jobs_.empty())
        return;
    Job &job = *jobs_.front();
    if (!job.rendering) {
        // Made the first time it is the one to render, then prepared once a frame until its sounds
        // and plugins are in
        if (!job.engine)
            job.engine = std::make_unique<OfflineEngine>(env_, plugins_, library_, pool_, job.snapshot, 1.0f, job.rate);
        std::string waiting;
        if (!job.engine->prepare(waiting)) {
            job.status = waiting.empty() ? std::string("Getting ready") : "Waiting for " + waiting;
            return;
        }
        job.latency = job.engine->latency();
        startRender(job);
        return;
    }
    if (!job.run->done.load())
        return;
    Result result;
    if (job.run->ok) {
        result.file = job.path;
        result.seconds = static_cast<double>(job.run->frames) / job.rate;
    } else if (job.run->cancelled || job.cancelling) {
        result.cancelled = true;
        std::error_code ec;
        fs::remove(fs::u8path(job.path), ec);
    } else {
        result.error = job.run->error.empty() ? std::string("The render failed") : job.run->error;
    }
    finish(job, std::move(result));
    jobs_.erase(jobs_.begin()); // waits for nothing: its render has finished
}

std::vector<Freezer::Result> Freezer::takeFinished()
{
    std::vector<Result> out;
    out.swap(finished_);
    return out;
}

bool Freezer::busy(ObjectId track) const
{
    return std::any_of(jobs_.begin(), jobs_.end(), [track](const std::unique_ptr<Job> &j) { return j->track == track; });
}

float Freezer::progress(ObjectId track) const
{
    for (const auto &job : jobs_) {
        if (job->track == track)
            return job->rendering && job->run && job->run->pass ? job->run->pass->fraction() : 0.0f;
    }
    return 0.0f;
}

std::string Freezer::status(ObjectId track) const
{
    for (const auto &job : jobs_) {
        if (job->track == track)
            return job->status;
    }
    return {};
}

void Freezer::cancel(ObjectId track)
{
    for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
        Job &job = **it;
        if (job.track != track)
            continue;
        if (!job.rendering) {
            // Not rendering yet: gone at once
            Result result;
            result.cancelled = true;
            finish(job, std::move(result));
            jobs_.erase(it);
            return;
        }
        // Rendering: it stops within a block, and update() reports it
        job.cancelling = true;
        if (job.run && job.run->pass)
            job.run->pass->cancel();
        return;
    }
}

}
