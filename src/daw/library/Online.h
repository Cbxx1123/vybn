#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "daw/library/Archive.h"
#include "daw/library/Library.h"
#include "daw/app/Settings.h"
#include "engine/Http.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

namespace daw {

// What an online asset becomes once downloaded: an instrument (an archive with an SFZ), one
// sound file, or a pack of sound files (an archive)
enum class AssetKind { Instrument, Sample, Pack };
const char *assetKindName(AssetKind kind);

// Something a source offers
struct OnlineItem {
    std::string id;          // unique within its source
    std::string source;      // the source's name
    std::string name;
    std::string description;
    std::string category;
    std::string author;
    std::string license;     // short: "CC0-1.0", "CC BY 4.0"
    std::string page;        // a web page about it
    AssetKind kind = AssetKind::Instrument;
    uint64_t bytes = 0;      // download size, about (0 when unknown)
    std::string url;         // what Download fetches
    std::string preview;     // a sound to listen to first, or ""
    std::string fileName;    // what a sample is saved as (with its extension)
    double seconds = 0.0;    // a sample's length
};

// Where online assets come from. Read from %APPDATA%\Vibin\sources.toml when it exists, so more
// can be added, else these defaults:
//   github:    every repository of a GitHub organisation, each an instrument downloaded as a zip
//              (sfzinstruments: free SFZ instruments, mostly CC0)
//   freesound: Freesound's search, one sound per result (its high-quality MP3 preview); needs a
//              free API key, set in Settings
//   catalogue: a TOML list of items (a web address or a file): [[items]] with name, kind
//              ("instrument", "sample", "pack"), url, and optionally category, license, author,
//              page, description, size (bytes), preview
struct SourceConfig {
    std::string name;
    std::string type;                 // "github", "freesound" or "catalogue"
    std::string org;                  // github
    std::string url;                  // catalogue
    std::vector<std::string> exclude; // github: repositories to leave out
    bool searchable() const { return type == "freesound"; } // asks the server for each search
};
std::vector<SourceConfig> defaultSources();
std::string sourcesPath();
// Reads sources.toml; on a missing file the defaults, on a broken one the defaults and a reason
std::vector<SourceConfig> loadSources(std::string &error);
bool readSources(const std::string &text, std::vector<SourceConfig> &sources, std::string &error);

// Listings into items (offline, for tests too)
std::vector<OnlineItem> parseGithubRepos(const std::string &json, const SourceConfig &source, std::string &error);
std::vector<OnlineItem> parseFreesound(const std::string &json, const SourceConfig &source, std::string &error);
std::vector<OnlineItem> parseCatalogue(const std::string &text, const SourceConfig &source, std::string &error);
// "karoryfer.272-merry-orks" -> "272 Merry Orks", "SalamanderGrandPiano" -> "Salamander Grand Piano"
std::string prettyRepoName(const std::string &repo);
// "http://creativecommons.org/licenses/by/4.0/" -> "CC BY 4.0"
std::string licenseName(const std::string &url);
// The Freesound search a category and text turn into
std::string freesoundQuery(const std::string &text, const std::string &category);

// The sources' listings. A listing is fetched on a background thread and arrives on the main
// thread; GitHub listings are kept for an hour (in %APPDATA%\Vibin\cache) to stay inside its
// limit for unsigned-in requests.
class OnlineCatalogue {
public:
    struct Listing {
        bool loading = false;
        std::string error;
        std::vector<OnlineItem> items;
        std::string query; // what a searchable source was last asked for
        uint64_t version = 0;
    };

    OnlineCatalogue(env::Environment &env, Settings &settings);
    ~OnlineCatalogue();

    OnlineCatalogue(const OnlineCatalogue &) = delete;
    OnlineCatalogue &operator=(const OnlineCatalogue &) = delete;

    const std::vector<SourceConfig> &sources() const { return sources_; }
    const std::string &sourcesError() const { return sourcesError_; }
    const Listing &listing(size_t source) const;
    // Fetches a source's listing unless it has one (force: again). A searchable source fetches
    // when the query changes.
    void fetch(size_t source, const std::string &query = "", bool force = false);
    // Goes up whenever any listing changes
    uint64_t version() const { return version_; }

    // Downloads an item's preview into the temp folder (once) and calls done(path, error) on
    // the main thread
    void preview(const OnlineItem &item, std::function<void(const std::string &, const std::string &)> done);

private:
    env::Environment &env_;
    Settings &settings_;
    std::vector<SourceConfig> sources_;
    std::string sourcesError_;
    std::vector<Listing> listings_;
    uint64_t version_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    env::JobGroup jobs_; // its background jobs, waited for when it goes
};

// Downloads: each on its own thread, into <library>/.downloads, unpacked when it is an archive,
// then moved into the library on the main thread (instruments into Instruments/<category>,
// sounds into Samples/<category>) with a note of where it came from and its licence.
class Downloads {
public:
    enum class State { Downloading, Unpacking, Installing, Done, Failed, Cancelled };
    struct Job {
        OnlineItem item;
        State state = State::Downloading;
        std::string error;
        std::string installed; // where it went
        std::shared_ptr<engine::HttpProgress> progress = std::make_shared<engine::HttpProgress>();
        std::shared_ptr<ExtractProgress> unpacking = std::make_shared<ExtractProgress>();
        // 0-1, for a progress bar
        float fraction() const;
        bool active() const { return state == State::Downloading || state == State::Unpacking || state == State::Installing; }
    };

    Downloads(env::Environment &env, InstrumentLibrary &library);
    ~Downloads(); // cancels what is running and waits for it

    Downloads(const Downloads &) = delete;
    Downloads &operator=(const Downloads &) = delete;

    // Starts one (again, if it failed or was cancelled); `done` runs on the main thread after it
    // is installed or fails
    void start(const OnlineItem &item, std::function<void(const Job &)> done = {});
    void cancel(const std::string &source, const std::string &id);
    // The latest job for an item, or nullptr
    const Job *find(const std::string &source, const std::string &id) const;
    int activeCount() const;
    std::string downloadFolder() const;

private:
    void install(Job &job, const std::string &file, const std::string &unpacked);

    env::Environment &env_;
    InstrumentLibrary &library_;
    std::vector<std::unique_ptr<Job>> jobs_;
    std::map<Job *, std::function<void(const Job &)>> callbacks_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    env::JobGroup work_; // its background jobs, waited for when it goes
    uint64_t serial_ = 0;
};

const char *downloadStateName(Downloads::State state);

}
