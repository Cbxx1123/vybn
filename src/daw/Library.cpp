#include "Library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

#include "audio/Memory.h"

namespace daw {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isAudioFile(const fs::path &p)
{
    const std::string ext = lower(p.extension().string());
    return ext == ".wav" || ext == ".flac" || ext == ".mp3";
}

bool copyFile(const fs::path &from, const fs::path &to, std::string &error)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
        error = "Can't copy " + from.string() + ": " + ec.message();
        return false;
    }
    return true;
}

// Renames, or copies and removes the original when the rename can't (another drive)
bool moveTo(const fs::path &from, const fs::path &to, std::string &error)
{
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    ec.clear();
    fs::copy(from, to, fs::copy_options::recursive, ec);
    if (ec) {
        error = "Can't move " + from.string() + " to " + to.string() + ": " + ec.message();
        return false;
    }
    fs::remove_all(from, ec);
    return true;
}

// A name not taken in `folder`: "Name", else "Name 2", "Name 3"...
fs::path freeName(const fs::path &folder, const std::string &name, const std::string &extension = "")
{
    fs::path p = folder / fs::u8path(name + extension);
    std::error_code ec;
    for (int n = 2; fs::exists(p, ec) && n < 1000; ++n)
        p = folder / fs::u8path(name + " " + std::to_string(n) + extension);
    return p;
}

// Whether a folder holds an instrument itself: an .sfz, or note-named audio files, directly in it
bool holdsInstrument(const fs::path &folder)
{
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const fs::path &p = entry.path();
        if (lower(p.extension().string()) == ".sfz")
            return true;
        if (isAudioFile(p) && audio::keyFromName(p.stem().string()) >= 0)
            return true;
    }
    return false;
}

// Whether it holds one at most two folders down (downloaded instruments often keep their SFZ
// in a subfolder)
bool containsInstrument(const fs::path &folder)
{
    return holdsInstrument(folder) || !audio::findSfz(folder.string()).empty();
}

// The default category matching a folder name, ignoring case, or the name itself
std::string categoryName(const std::string &folder)
{
    for (const std::string &c : defaultCategories()) {
        if (lower(c) == lower(folder))
            return c;
    }
    return folder;
}

}

const std::vector<std::string> &defaultCategories()
{
    static const std::vector<std::string> categories{"Guitars", "Bass",  "Keys",   "Drums",         "Percussion",
                                                     "Strings", "Brass", "Winds",  "Synths",        "Vocals",
                                                     "Loops",   "Effects", "Other"};
    return categories;
}

std::string guessCategory(const std::string &text)
{
    const std::string t = lower(text);
    // Earlier entries win: "bass guitar" is Bass, "drum loop" is Loops
    static const std::vector<std::pair<const char *, std::vector<const char *>>> words{
        {"Loops", {"loop", "groove", "break"}},
        {"Bass", {"bass", "808"}},
        {"Guitars", {"guitar", "nylon", "acoustic", "strat", "uke", "banjo", "mandolin", "sitar", "harp", "lute"}},
        {"Keys", {"piano", "rhodes", "wurli", "organ", "keys", "clav", "harpsichord", "celesta", "e-piano", "pianet",
                  "grand"}},
        {"Drums", {"drum", "kick", "snare", "hihat", "hi-hat", "cymbal", "tom", "kit"}},
        {"Percussion", {"perc", "shaker", "cabasa", "conga", "bongo", "tabla", "marimba", "xylo", "vibraphone",
                        "glock", "bell", "clap", "tambourine", "cowbell", "hang", "gong", "phu", "gogodze", "chime"}},
        {"Strings", {"violin", "viola", "cello", "string", "fiddle", "orchestra"}},
        {"Brass", {"trumpet", "trombone", "tuba", "horn", "brass", "cornet"}},
        {"Winds", {"sax", "flute", "clarinet", "oboe", "bassoon", "recorder", "pipe", "whistle", "harmonica",
                   "accordion"}},
        {"Synths", {"synth", "pad", "lead", "pluck", "chip", "square", "saw", "analog", "cosmonaut", "moog"}},
        {"Vocals", {"vocal", "voice", "choir", "vox", "sing", "ork"}},
        {"Effects", {"fx", "effect", "riser", "noise", "impact", "sweep", "foley", "ambience", "atmo"}},
    };
    for (const auto &[category, keys] : words) {
        for (const char *k : keys) {
            if (t.find(k) != std::string::npos)
                return category;
        }
    }
    return "Other";
}

std::string safeFileName(std::string name, const std::string &fallback)
{
    for (char &c : name) {
        if (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32)
            c = '-';
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.size() > 120)
        name.resize(120);
    return name.empty() ? fallback : name;
}

InstrumentLibrary::InstrumentLibrary(env::Environment &env, Settings &settings)
    : env_(env), settings_(settings), resources_(env.find<env::ResourceManager>())
{
    if (resources_)
        resources_->add(cache_);
    rescan();
}

InstrumentLibrary::~InstrumentLibrary()
{
    jobs_.wait();
    if (resources_)
        resources_->remove(cache_);
}

std::string InstrumentLibrary::instrumentsFolder() const
{
    return (fs::u8path(settings_.libraryPath) / "Instruments").string();
}

std::string InstrumentLibrary::samplesFolder() const
{
    return (fs::u8path(settings_.libraryPath) / "Samples").string();
}

const LibraryInstrument *InstrumentLibrary::find(const std::string &name) const
{
    for (const LibraryInstrument &e : entries_) {
        if (e.name == name)
            return &e;
    }
    return nullptr;
}

std::vector<std::string> InstrumentLibrary::categories() const
{
    std::vector<std::string> out = defaultCategories();
    auto note = [&out](const std::string &c) {
        if (std::find(out.begin(), out.end(), c) == out.end())
            out.push_back(c);
    };
    for (const LibraryInstrument &e : entries_)
        note(e.category);
    for (const LibrarySample &s : samples_)
        note(s.category);
    return out;
}

void InstrumentLibrary::rescan()
{
    std::vector<LibraryInstrument> entries;
    auto add = [&entries](const fs::path &folder, const std::string &category) {
        const std::string name = folder.filename().string();
        for (const LibraryInstrument &e : entries) {
            if (e.name == name)
                return; // names are what projects use: the first one wins
        }
        entries.push_back({name, category, folder.string()});
    };
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(fs::u8path(instrumentsFolder()), ec)) {
        if (!entry.is_directory(ec))
            continue;
        const fs::path &folder = entry.path();
        const std::string dirName = folder.filename().string();
        if (!dirName.empty() && dirName[0] == '.')
            continue;
        if (holdsInstrument(folder)) {
            add(folder, guessCategory(dirName)); // an instrument directly in Instruments
            continue;
        }
        // A category folder
        std::error_code inner;
        for (const auto &child : fs::directory_iterator(folder, inner)) {
            if (child.is_directory(inner) && containsInstrument(child.path()))
                add(child.path(), categoryName(dirName));
        }
    }
    std::sort(entries.begin(), entries.end(),
              [](const LibraryInstrument &a, const LibraryInstrument &b) { return lower(a.name) < lower(b.name); });

    std::vector<LibrarySample> samples;
    const fs::path root = fs::u8path(samplesFolder());
    ec.clear();
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator() && samples.size() < 20000; it.increment(ec)) {
        const fs::path &p = it->path();
        if (it.depth() > 6)
            it.disable_recursion_pending();
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc) || !isAudioFile(p))
            continue;
        const fs::path relative = p.lexically_relative(root);
        auto part = relative.begin();
        LibrarySample s;
        s.name = p.stem().string();
        s.path = p.string();
        s.bytes = it->file_size(fileEc);
        if (std::distance(relative.begin(), relative.end()) == 1) {
            s.category = guessCategory(s.name);
        } else {
            s.category = categoryName(part->string());
            ++part;
            if (std::distance(part, relative.end()) > 1)
                s.pack = part->string();
        }
        samples.push_back(std::move(s));
    }
    std::sort(samples.begin(), samples.end(), [](const LibrarySample &a, const LibrarySample &b) {
        return a.pack != b.pack ? lower(a.pack) < lower(b.pack) : lower(a.name) < lower(b.name);
    });

    std::string stamp;
    for (const LibraryInstrument &e : entries)
        stamp += e.folder + "|";
    for (const LibrarySample &s : samples)
        stamp += s.path + "|";
    if (stamp != stamp_) {
        stamp_ = std::move(stamp);
        ++version_;
    }
    entries_ = std::move(entries);
    samples_ = std::move(samples);
    names_.clear();
    for (const LibraryInstrument &e : entries_)
        names_.push_back(e.name);
}

std::string InstrumentLibrary::import(const std::vector<std::string> &files, std::string &error,
                                      const std::string &category)
{
    if (files.empty()) {
        error = "Nothing to import";
        return {};
    }
    const fs::path root = fs::u8path(instrumentsFolder());
    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) {
        error = "Can't create " + root.string() + ": " + ec.message();
        return {};
    }

    std::string name;
    const fs::path first = fs::u8path(files.front());
    if (files.size() == 1 && lower(first.extension().string()) == ".sfz") {
        // An SFZ, the files it includes and the samples it refers to, keeping their places
        std::string display;
        std::vector<std::string> includes;
        const std::vector<std::string> samples = audio::sfzSamplePaths(first.string(), display, error, &includes);
        if (!error.empty())
            return {};
        if (samples.empty()) {
            error = first.filename().string() + " refers to no samples";
            return {};
        }
        name = safeFileName(display.empty() ? first.stem().string() : display, "Instrument");
        if (find(name)) {
            error = "\"" + name + "\" is already in the library";
            return {};
        }
        const fs::path target = root / fs::u8path(category.empty() ? guessCategory(name) : category) / fs::u8path(name);
        std::vector<std::string> copies = samples;
        copies.insert(copies.end(), includes.begin(), includes.end());
        for (const std::string &relative : copies) {
            const fs::path from = first.parent_path() / fs::u8path(relative).make_preferred();
            if (!copyFile(from, target / fs::u8path(relative).make_preferred(), error))
                return {};
        }
        if (!copyFile(first, target / first.filename(), error))
            return {};
        // Licence and readme files beside the SFZ come too
        for (const auto &entry : fs::directory_iterator(first.parent_path(), ec)) {
            const std::string ext = lower(entry.path().extension().string());
            if (entry.is_regular_file() && (ext == ".txt" || ext == ".md"))
                copyFile(entry.path(), target / entry.path().filename(), error);
        }
        error.clear();
    } else {
        // Note-named audio files
        std::vector<fs::path> audio;
        for (const std::string &f : files) {
            const fs::path p = fs::u8path(f);
            if (isAudioFile(p) && audio::keyFromName(p.stem().string()) >= 0)
                audio.push_back(p);
        }
        if (audio.empty()) {
            error = "Choose an .sfz file, or audio files named by their note (like E2.wav)";
            return {};
        }
        name = safeFileName(first.parent_path().filename().string(), "Instrument");
        if (find(name)) {
            error = "\"" + name + "\" is already in the library";
            return {};
        }
        const fs::path target = root / fs::u8path(category.empty() ? guessCategory(name) : category) / fs::u8path(name);
        for (const fs::path &p : audio) {
            if (!copyFile(p, target / p.filename(), error))
                return {};
        }
    }
    rescan();
    return name;
}

int InstrumentLibrary::importSamples(const std::vector<std::string> &files, const std::string &category,
                                     std::string &error)
{
    int copied = 0;
    for (const std::string &f : files) {
        const fs::path from = fs::u8path(f);
        if (!isAudioFile(from))
            continue;
        const std::string cat = category.empty() ? guessCategory(from.stem().string()) : category;
        const fs::path folder = fs::u8path(samplesFolder()) / fs::u8path(cat);
        const fs::path to = freeName(folder, from.stem().string(), from.extension().string());
        if (!copyFile(from, to, error))
            break;
        ++copied;
    }
    if (copied == 0 && error.empty())
        error = "Choose sound files (WAV, FLAC or MP3)";
    rescan();
    return copied;
}

std::string InstrumentLibrary::installInstrument(const std::string &folder, const std::string &name,
                                                 const std::string &category, std::string &error)
{
    std::string base = safeFileName(name, "Instrument");
    // Names are unique across categories
    std::string unique = base;
    for (int n = 2; find(unique) && n < 1000; ++n)
        unique = base + " " + std::to_string(n);
    const fs::path categoryFolder = fs::u8path(instrumentsFolder()) / fs::u8path(category.empty() ? "Other" : category);
    const fs::path target = freeName(categoryFolder, unique);
    if (!moveTo(fs::u8path(folder), target, error))
        return {};
    rescan();
    return target.string();
}

std::string InstrumentLibrary::installSamples(const std::string &path, const std::string &name,
                                              const std::string &category, std::string &error)
{
    const fs::path from = fs::u8path(path);
    const fs::path folder = fs::u8path(samplesFolder()) / fs::u8path(category.empty() ? "Other" : category);
    std::error_code ec;
    const bool isFolder = fs::is_directory(from, ec);
    const fs::path target = freeName(folder, safeFileName(name, "Sample"), isFolder ? "" : from.extension().string());
    if (!moveTo(from, target, error))
        return {};
    rescan();
    return target.string();
}

bool InstrumentLibrary::moveInstrument(const std::string &name, const std::string &category, std::string &error)
{
    const LibraryInstrument *e = find(name);
    if (!e) {
        error = "\"" + name + "\" isn't in the library";
        return false;
    }
    if (loading(name)) {
        error = "\"" + name + "\" is loading; try again in a moment";
        return false;
    }
    if (e->category == category)
        return true;
    const fs::path target = fs::u8path(instrumentsFolder()) / fs::u8path(category) / fs::u8path(name);
    std::error_code ec;
    if (fs::exists(target, ec)) {
        error = category + " already has a \"" + name + "\"";
        return false;
    }
    if (!moveTo(fs::u8path(e->folder), target, error))
        return false;
    rescan();
    return true;
}

bool InstrumentLibrary::moveSample(const std::string &path, const std::string &category, std::string &error)
{
    const fs::path from = fs::u8path(path);
    const fs::path folder = fs::u8path(samplesFolder()) / fs::u8path(category);
    if (from.parent_path() == folder)
        return true;
    if (!moveTo(from, freeName(folder, from.stem().string(), from.extension().string()), error))
        return false;
    rescan();
    return true;
}

std::shared_ptr<const audio::SampleSet> InstrumentLibrary::cached(const std::string &name) const
{
    return cache_.find(name);
}

void InstrumentLibrary::load(const std::string &name, Loaded done)
{
    if (auto set = cached(name)) {
        done(set, {});
        return;
    }
    const LibraryInstrument *entry = find(name);
    if (!entry) {
        done(nullptr, "\"" + name + "\" isn't in the library");
        return;
    }
    auto &waiting = pending_[name];
    waiting.push_back(std::move(done));
    if (waiting.size() > 1)
        return; // already loading

    const std::string folder = entry->folder;
    env::Environment &env = env_;
    // Long samples stream from disk when the program has a memory manager that streams
    audio::MemoryManager *memory = env.find<audio::MemoryManager>();
    env::runJob(env_, env::Lane::Interactive, [this, &env, name, folder, memory] {
        std::string error;
        std::shared_ptr<const audio::SampleSet> set = audio::loadInstrumentFolder(folder, error, memory);
        env.post([this, name, set, error] {
            if (set)
                cache_.put(name, set);
            auto callbacks = std::move(pending_[name]);
            pending_.erase(name);
            for (auto &callback : callbacks)
                callback(set, error);
        });
    }, jobs_);
}

}
