#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "daw/app/Settings.h"
#include "audio/SampleSet.h"
#include "env/Environment.h"
#include "env/Resources.h"
#include "env/Scheduler.h"

namespace daw {

// The categories the library is sorted into, in the order the browser lists them. A folder with
// another name is a category too.
const std::vector<std::string> &defaultCategories();
// A category for something from its name and description ("Nylon guitar" is Guitars), or "Other"
std::string guessCategory(const std::string &text);
// A name that works as a file or folder name everywhere ("" becomes `fallback`)
std::string safeFileName(std::string name, const std::string &fallback = "Untitled");

struct LibraryInstrument {
    std::string name;     // its folder's name, which projects refer to it by
    std::string category;
    std::string folder;   // full path
};

struct LibrarySample {
    std::string name;     // the file's name without its extension
    std::string category;
    std::string pack;     // the folder it is in under its category ("" when directly in it)
    std::string path;     // full path
    uint64_t bytes = 0;
};

// The sample library on disk, under the folder in the Settings:
//   Instruments/<Category>/<Instrument>/  an SFZ with its samples, or note-named audio files
//   Samples/<Category>/[<Pack>/...]       sound files (one-shots, loops) to put on audio tracks
// Instrument folders directly in Instruments (as earlier versions made them) still count; their
// category is guessed from the name. Instrument names are unique: the first found wins.
//
// Instruments load on a background thread; the result arrives on the main thread through
// Environment::post. Loaded sets are cached by name and shared by every track that plays them.
class InstrumentLibrary {
public:
    using Loaded = std::function<void(std::shared_ptr<const audio::SampleSet> set, const std::string &error)>;

    InstrumentLibrary(env::Environment &env, Settings &settings);
    ~InstrumentLibrary(); // waits for loads in progress

    InstrumentLibrary(const InstrumentLibrary &) = delete;
    InstrumentLibrary &operator=(const InstrumentLibrary &) = delete;

    std::string instrumentsFolder() const;
    std::string samplesFolder() const;
    // Instrument names, sorted; rescan() rereads the folders
    const std::vector<std::string> &instruments() const { return names_; }
    const std::vector<LibraryInstrument> &entries() const { return entries_; }
    const std::vector<LibrarySample> &samples() const { return samples_; }
    const LibraryInstrument *find(const std::string &name) const;
    // The default categories, then any other category folders found
    std::vector<std::string> categories() const;
    void rescan();
    // Goes up whenever a rescan finds something different, so views know to rebuild
    uint64_t version() const { return version_; }

    // Copies an instrument into the library and returns its name ("" on failure, with the
    // reason in `error`). Either one .sfz file, copied with every sample and file it refers to,
    // or audio files named by note ("E2.wav"), copied into a folder named after the one they're
    // in. It goes in `category`, or one guessed from its name.
    std::string import(const std::vector<std::string> &files, std::string &error, const std::string &category = "");
    // Copies sound files into Samples/<category>; returns how many
    int importSamples(const std::vector<std::string> &files, const std::string &category, std::string &error);
    // Moves a folder (a downloaded instrument, unpacked) into Instruments/<category>/<name>, or a
    // file or folder of sounds into Samples/<category>; returns the path it went to ("" on failure)
    std::string installInstrument(const std::string &folder, const std::string &name, const std::string &category,
                                  std::string &error);
    std::string installSamples(const std::string &path, const std::string &name, const std::string &category,
                               std::string &error);
    // Moves an instrument (by name) or a sample (by path) to another category
    bool moveInstrument(const std::string &name, const std::string &category, std::string &error);
    bool moveSample(const std::string &path, const std::string &category, std::string &error);

    // Calls `done` on the main thread with the instrument's samples: at once when they're
    // cached, otherwise once a background thread has loaded them
    void load(const std::string &name, Loaded done);
    // Whether `name` is loading now
    bool loading(const std::string &name) const { return pending_.count(name) > 0; }
    // Whether any set is loading now
    bool anyLoading() const { return !pending_.empty(); }
    // The cached set, or nullptr. Loaded sets stay while a track (or a preview) plays them; once
    // nothing has for a while the ResourceManager lets them go.
    std::shared_ptr<const audio::SampleSet> cached(const std::string &name) const;

private:
    env::Environment &env_;
    Settings &settings_;
    std::vector<std::string> names_;
    std::vector<LibraryInstrument> entries_;
    std::vector<LibrarySample> samples_;
    uint64_t version_ = 0;
    std::string stamp_; // what the last rescan found, to tell when it changes
    // Asking for a set counts as using it, hence mutable
    mutable env::ResourceCache<const audio::SampleSet> cache_{"Instruments", [](const audio::SampleSet &s) { return s.bytes(); }};
    env::ResourceManager *resources_ = nullptr;
    std::map<std::string, std::vector<Loaded>> pending_;
    env::JobGroup jobs_; // its background jobs, waited for when it goes
};

}
