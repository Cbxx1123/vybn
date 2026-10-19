#include "Online.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "Json.h"
#include "Toml.h"

namespace daw {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool writeFile(const fs::path &path, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

fs::path configFolder() { return fs::u8path(Settings::filePath()).parent_path(); }

std::string trimTo(std::string s, size_t length)
{
    for (char &c : s) {
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
    }
    if (s.size() > length) {
        s.resize(length);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
            s.pop_back(); // not in the middle of a character
        s += "...";
    }
    return s;
}

AssetKind kindFromName(const std::string &name)
{
    const std::string n = lower(name);
    if (n == "sample" || n == "sound")
        return AssetKind::Sample;
    if (n == "pack" || n == "samples")
        return AssetKind::Pack;
    return AssetKind::Instrument;
}

// The extension a download is saved with, from its address ("" when it has none)
std::string extensionOf(const std::string &url)
{
    std::string path = url.substr(0, url.find_first_of("?#"));
    const size_t slash = path.find_last_of('/');
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string l = lower(file);
    for (const char *ext : {".tar.gz", ".tar.bz2", ".tar.xz"}) {
        if (l.size() > std::string(ext).size() && l.compare(l.size() - std::string(ext).size(), std::string::npos, ext) == 0)
            return ext;
    }
    const size_t dot = l.find_last_of('.');
    return dot == std::string::npos || l.size() - dot > 6 ? std::string() : l.substr(dot);
}

}

const char *assetKindName(AssetKind kind)
{
    switch (kind) {
    case AssetKind::Instrument: return "Instrument";
    case AssetKind::Sample: return "Sample";
    case AssetKind::Pack: return "Sample pack";
    }
    return "";
}

const char *downloadStateName(Downloads::State state)
{
    switch (state) {
    case Downloads::State::Downloading: return "Downloading";
    case Downloads::State::Unpacking: return "Unpacking";
    case Downloads::State::Installing: return "Installing";
    case Downloads::State::Done: return "Installed";
    case Downloads::State::Failed: return "Failed";
    case Downloads::State::Cancelled: return "Cancelled";
    }
    return "";
}

// Sources

std::vector<SourceConfig> defaultSources()
{
    SourceConfig github;
    github.name = "SFZ Instruments (GitHub)";
    github.type = "github";
    github.org = "sfzinstruments";
    github.exclude = {"mappings", "sfzinstruments.github.io"};
    SourceConfig freesound;
    freesound.name = "Freesound";
    freesound.type = "freesound";
    return {github, freesound};
}

std::string sourcesPath() { return (configFolder() / "sources.toml").string(); }

bool readSources(const std::string &text, std::vector<SourceConfig> &sources, std::string &error)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return false;
    error.clear();
    const toml::Array *list = root.arrayAt("sources");
    if (!list) {
        error = "it has no [[sources]]";
        return false;
    }
    sources.clear();
    for (const toml::Value &s : *list) {
        if (!s.isTable())
            continue;
        SourceConfig c;
        c.type = lower(s.get("type", ""));
        c.name = s.get("name", "");
        c.org = s.get("org", "");
        c.url = s.get("url", "");
        if (const toml::Array *ex = s.arrayAt("exclude")) {
            for (const toml::Value &v : *ex) {
                if (v.type() == toml::Type::String)
                    c.exclude.push_back(v.string());
            }
        }
        if (c.type == "github" && c.org.empty()) {
            error = "a github source needs an org";
            continue;
        }
        if (c.type == "catalogue" && c.url.empty()) {
            error = "a catalogue source needs a url";
            continue;
        }
        if (c.type != "github" && c.type != "freesound" && c.type != "catalogue") {
            error = "unknown source type \"" + c.type + "\"";
            continue;
        }
        if (c.name.empty())
            c.name = c.type == "github" ? c.org : c.type == "freesound" ? "Freesound" : c.url;
        sources.push_back(std::move(c));
    }
    return error.empty();
}

std::vector<SourceConfig> loadSources(std::string &error)
{
    const std::string text = readFile(fs::u8path(sourcesPath()));
    if (text.empty())
        return defaultSources();
    std::vector<SourceConfig> sources;
    if (!readSources(text, sources, error)) {
        error = "sources.toml: " + error;
        if (sources.empty())
            return defaultSources();
    }
    return sources;
}

std::string prettyRepoName(const std::string &repo)
{
    std::string base = repo;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot + 1 < base.size())
        base = base.substr(dot + 1);
    std::string words;
    for (size_t i = 0; i < base.size(); ++i) {
        const char c = base[i];
        if (c == '-' || c == '_') {
            words += ' ';
            continue;
        }
        // Split CamelCase: before an upper-case letter after a lower-case one, or before the last
        // capital of a run that starts a word ("DRSKit" -> "DRS Kit")
        if (i > 0 && std::isupper(static_cast<unsigned char>(c))) {
            const char prev = base[i - 1];
            const bool next = i + 1 < base.size() && std::islower(static_cast<unsigned char>(base[i + 1]));
            if (std::islower(static_cast<unsigned char>(prev)) || std::isdigit(static_cast<unsigned char>(prev)) ||
                (std::isupper(static_cast<unsigned char>(prev)) && next))
                words += ' ';
        }
        words += c;
    }
    // Capital first letters
    bool start = true;
    for (char &c : words) {
        if (start && std::islower(static_cast<unsigned char>(c)))
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        start = c == ' ';
    }
    return words;
}

std::string licenseName(const std::string &url)
{
    const std::string u = lower(url);
    if (u.find("publicdomain/zero") != std::string::npos || u.find("cc0") != std::string::npos)
        return "CC0";
    if (u.find("creativecommons.org/licenses/") != std::string::npos) {
        // .../licenses/by-nc/3.0/
        const size_t start = u.find("/licenses/") + 10;
        const size_t slash = u.find('/', start);
        std::string terms = u.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        std::string version;
        if (slash != std::string::npos) {
            const size_t end = u.find('/', slash + 1);
            version = u.substr(slash + 1, end == std::string::npos ? std::string::npos : end - slash - 1);
        }
        for (char &c : terms)
            c = c == '-' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return "CC " + terms + (version.empty() ? "" : " " + version);
    }
    if (u.find("sampling+") != std::string::npos)
        return "Sampling+";
    return url;
}

std::vector<OnlineItem> parseGithubRepos(const std::string &json, const SourceConfig &source, std::string &error)
{
    toml::Value root;
    if (!json::parse(json, root, error))
        return {};
    if (!root.isArray()) {
        error = root.get("message", "GitHub answered with something other than a list");
        return {};
    }
    std::vector<OnlineItem> items;
    for (const toml::Value &repo : root.array()) {
        if (!repo.isTable())
            continue;
        const std::string name = repo.get("name", "");
        if (name.empty() || repo.get("archived", false) || repo.get("fork", false))
            continue;
        if (std::find(source.exclude.begin(), source.exclude.end(), name) != source.exclude.end())
            continue;
        const int64_t kb = repo.get("size", static_cast<int64_t>(0));
        if (kb < 64)
            continue; // too small to hold samples
        OnlineItem item;
        item.id = name;
        item.source = source.name;
        item.name = prettyRepoName(name);
        item.description = trimTo(repo.get("description", ""), 400);
        const size_t dot = name.find('.');
        item.author = dot != std::string::npos ? name.substr(0, dot) : source.org;
        item.category = guessCategory(item.name + " " + item.description);
        if (const toml::Value *license = repo.at("license"); license && license->isTable()) {
            item.license = license->get("spdx_id", "");
            if (item.license.empty() || item.license == "NOASSERTION")
                item.license = license->get("name", "");
            if (item.license == "Other")
                item.license = "See its licence file";
        }
        if (item.license.empty())
            item.license = "Not stated: check its page";
        item.page = repo.get("html_url", "https://github.com/" + source.org + "/" + name);
        item.kind = AssetKind::Instrument;
        item.bytes = static_cast<uint64_t>(kb) * 1024;
        const std::string branch = repo.get("default_branch", "master");
        item.url = "https://codeload.github.com/" + source.org + "/" + name + "/zip/refs/heads/" + branch;
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const OnlineItem &a, const OnlineItem &b) { return lower(a.name) < lower(b.name); });
    return items;
}

std::vector<OnlineItem> parseFreesound(const std::string &json, const SourceConfig &source, std::string &error)
{
    toml::Value root;
    if (!json::parse(json, root, error))
        return {};
    const toml::Array *results = root.arrayAt("results");
    if (!results) {
        error = root.get("detail", "Freesound answered with something unexpected");
        return {};
    }
    std::vector<OnlineItem> items;
    for (const toml::Value &r : *results) {
        if (!r.isTable())
            continue;
        OnlineItem item;
        const int64_t id = r.get("id", static_cast<int64_t>(0));
        if (id == 0)
            continue;
        item.id = std::to_string(id);
        item.source = source.name;
        std::string name = r.get("name", "Sound " + item.id);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos && name.size() - dot <= 5)
            name.erase(dot);
        item.name = trimTo(name, 80);
        item.description = trimTo(r.get("description", ""), 300);
        item.author = r.get("username", "");
        item.license = licenseName(r.get("license", ""));
        item.page = r.get("url", "https://freesound.org/s/" + item.id + "/");
        std::string tags;
        if (const toml::Array *t = r.arrayAt("tags")) {
            for (const toml::Value &tag : *t) {
                if (tag.type() == toml::Type::String)
                    tags += " " + tag.string();
            }
        }
        item.category = guessCategory(item.name + tags);
        item.kind = AssetKind::Sample;
        item.seconds = r.get("duration", 0.0);
        if (const toml::Value *previews = r.at("previews"); previews && previews->isTable()) {
            item.url = previews->get("preview-hq-mp3", "");
            item.preview = item.url;
        }
        item.fileName = safeFileName(item.name, "Sound " + item.id) + ".mp3";
        if (!item.url.empty())
            items.push_back(std::move(item));
    }
    return items;
}

std::vector<OnlineItem> parseCatalogue(const std::string &text, const SourceConfig &source, std::string &error)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return {};
    error.clear();
    const toml::Array *list = root.arrayAt("items");
    if (!list) {
        error = "The catalogue has no [[items]]";
        return {};
    }
    std::vector<OnlineItem> items;
    for (const toml::Value &v : *list) {
        if (!v.isTable())
            continue;
        OnlineItem item;
        item.name = v.get("name", "");
        item.url = v.get("url", "");
        if (item.name.empty() || item.url.empty())
            continue;
        item.id = v.get("id", item.url);
        item.source = source.name;
        item.kind = kindFromName(v.get("kind", "instrument"));
        item.description = trimTo(v.get("description", ""), 400);
        item.author = v.get("author", "");
        item.license = v.get("license", "");
        item.page = v.get("page", "");
        item.preview = v.get("preview", "");
        item.bytes = static_cast<uint64_t>(std::max<int64_t>(0, v.get("size", static_cast<int64_t>(0))));
        item.category = v.get("category", guessCategory(item.name + " " + item.description));
        if (item.kind == AssetKind::Sample) {
            std::string ext = extensionOf(item.url);
            item.fileName = safeFileName(item.name, "Sound") + (ext.empty() ? ".wav" : ext);
        }
        items.push_back(std::move(item));
    }
    return items;
}

std::string freesoundQuery(const std::string &text, const std::string &category)
{
    std::string query = text;
    if (!category.empty() && category != "Other") {
        // The category as a word Freesound's tags use
        std::string word = lower(category);
        if (word == "keys")
            word = "piano";
        else if (word == "effects")
            word = "fx";
        else if (word.size() > 3 && word.back() == 's' && word != "bass")
            word.pop_back(); // "drums" -> "drum"
        query = query.empty() ? word : query + " " + word;
    }
    return query.empty() ? std::string("one shot") : query;
}

// OnlineCatalogue

OnlineCatalogue::OnlineCatalogue(env::Environment &env, Settings &settings) : env_(env), settings_(settings)
{
    sources_ = loadSources(sourcesError_);
    listings_.resize(sources_.size());
}

OnlineCatalogue::~OnlineCatalogue()
{
    *alive_ = false;
    jobs_.wait();
}

const OnlineCatalogue::Listing &OnlineCatalogue::listing(size_t source) const
{
    static const Listing none;
    return source < listings_.size() ? listings_[source] : none;
}

void OnlineCatalogue::fetch(size_t index, const std::string &query, bool force)
{
    if (index >= sources_.size())
        return;
    Listing &l = listings_[index];
    const SourceConfig source = sources_[index];
    if (l.loading)
        return;
    if (!force && l.version > 0 && (!source.searchable() || l.query == query))
        return;
    l.loading = true;
    l.error.clear();
    l.query = query;
    ++version_;

    if (source.type == "freesound" && settings_.freesoundKey.empty()) {
        l.loading = false;
        l.items.clear();
        l.error = "Freesound needs a free API key: get one at freesound.org/apiv2/apply, then add it in Settings.";
        ++l.version;
        ++version_;
        return;
    }

    const std::string key = settings_.freesoundKey;
    auto alive = alive_;
    env::Environment &env = env_;
    env::runJob(env_, env::Lane::Network, [this, alive, &env, index, source, query, force, key] {
        std::string error, body;
        std::vector<OnlineItem> items;
        engine::HttpRequest request;
        if (source.type == "github") {
            // Kept for an hour: GitHub allows 60 unsigned-in requests an hour
            const fs::path cache = configFolder() / "cache" / ("github-" + safeFileName(source.org) + ".json");
            std::error_code ec;
            const auto age = fs::exists(cache, ec) ? fs::file_time_type::clock::now() - fs::last_write_time(cache, ec)
                                                   : fs::file_time_type::duration::max();
            if (!force && age < std::chrono::hours(1))
                body = readFile(cache);
            if (body.empty()) {
                request.url = "https://api.github.com/orgs/" + source.org + "/repos?per_page=100&type=public";
                request.headers = {{"Accept", "application/vnd.github+json"}};
                if (engine::httpGetText(request, body, error))
                    writeFile(cache, body);
                else if (!(body = readFile(cache)).empty())
                    error.clear(); // offline: the old listing will do
            }
            if (error.empty())
                items = parseGithubRepos(body, source, error);
        } else if (source.type == "freesound") {
            request.url = "https://freesound.org/apiv2/search/text/?query=" + engine::urlEncode(query) +
                          "&page_size=40&fields=id,name,tags,license,username,duration,previews,description,url" +
                          "&filter=" + engine::urlEncode("duration:[0 TO 30]");
            request.headers = {{"Authorization", "Token " + key}};
            if (engine::httpGetText(request, body, error))
                items = parseFreesound(body, source, error);
        } else {
            const bool web = source.url.rfind("http://", 0) == 0 || source.url.rfind("https://", 0) == 0;
            if (web) {
                request.url = source.url;
                engine::httpGetText(request, body, error);
            } else {
                body = readFile(fs::u8path(source.url));
                if (body.empty())
                    error = "Can't read " + source.url;
            }
            if (error.empty())
                items = parseCatalogue(body, source, error);
        }
        env.post([this, alive, index, items = std::move(items), error] {
            if (!*alive)
                return;
            Listing &l = listings_[index];
            l.loading = false;
            l.error = error;
            l.items = items;
            ++l.version;
            ++version_;
        });
    }, jobs_);
}

void OnlineCatalogue::preview(const OnlineItem &item, std::function<void(const std::string &, const std::string &)> done)
{
    if (item.preview.empty()) {
        done({}, "No preview for this one");
        return;
    }
    const fs::path folder = fs::temp_directory_path() / "Vibin" / "previews";
    std::string ext = extensionOf(item.preview);
    const fs::path file = folder / (safeFileName(item.source + "-" + item.id) + (ext.empty() ? ".mp3" : ext));
    std::error_code ec;
    if (fs::exists(file, ec)) {
        done(file.string(), {});
        return;
    }
    auto alive = alive_;
    env::Environment &env = env_;
    const std::string url = item.preview;
    env::runJob(env_, env::Lane::Network, [alive, &env, url, file, done = std::move(done)] {
        std::string error;
        engine::HttpRequest request;
        request.url = url;
        const bool ok = engine::httpDownload(request, file.string(), error);
        env.post([alive, ok, file, error, done] {
            if (*alive)
                done(ok ? file.string() : std::string(), error);
        });
    }, jobs_);
}

// Downloads

float Downloads::Job::fraction() const
{
    if (state == State::Done)
        return 1.0f;
    if (state == State::Unpacking) {
        const uint64_t total = unpacking->total;
        return total ? 0.8f + 0.2f * static_cast<float>(unpacking->done) / static_cast<float>(total) : 0.8f;
    }
    if (state == State::Installing)
        return 0.99f;
    const uint64_t total = progress->total ? progress->total.load() : item.bytes;
    const bool archive = item.kind != AssetKind::Sample;
    const float share = archive ? 0.8f : 1.0f;
    return total ? std::min(share, share * static_cast<float>(progress->done) / static_cast<float>(total)) : 0.0f;
}

Downloads::Downloads(env::Environment &env, InstrumentLibrary &library) : env_(env), library_(library)
{
    // Downloads under way stop when the program does, rather than holding up its threads
    if (env::Scheduler *scheduler = env.find<env::Scheduler>()) {
        scheduler->onStop([this] {
            for (auto &job : jobs_)
                job->progress->cancel = true;
        });
    }
}

Downloads::~Downloads()
{
    *alive_ = false;
    for (auto &job : jobs_)
        job->progress->cancel = true;
    work_.wait();
}

std::string Downloads::downloadFolder() const
{
    return (fs::u8path(library_.instrumentsFolder()).parent_path() / ".downloads").string();
}

const Downloads::Job *Downloads::find(const std::string &source, const std::string &id) const
{
    for (auto it = jobs_.rbegin(); it != jobs_.rend(); ++it) {
        if ((*it)->item.source == source && (*it)->item.id == id)
            return it->get();
    }
    return nullptr;
}

int Downloads::activeCount() const
{
    return static_cast<int>(std::count_if(jobs_.begin(), jobs_.end(), [](const auto &j) { return j->active(); }));
}

void Downloads::cancel(const std::string &source, const std::string &id)
{
    for (auto &job : jobs_) {
        if (job->item.source == source && job->item.id == id && job->active()) {
            job->progress->cancel = true;
            job->unpacking->cancel = &job->progress->cancel;
        }
    }
}

void Downloads::start(const OnlineItem &item, std::function<void(const Job &)> done)
{
    if (const Job *existing = find(item.source, item.id); existing && (existing->active() || existing->state == State::Done))
        return;
    auto owned = std::make_unique<Job>();
    Job *job = owned.get();
    job->item = item;
    job->unpacking->cancel = &job->progress->cancel;
    jobs_.push_back(std::move(owned));
    if (done)
        callbacks_[job] = std::move(done);

    const fs::path folder = fs::u8path(downloadFolder());
    const std::string stem = std::to_string(++serial_) + "-" + safeFileName(item.source + " " + item.id, "download");
    std::string ext = extensionOf(item.url);
    if (item.kind != AssetKind::Sample && ext.empty())
        ext = ".zip"; // GitHub's archive addresses have no extension
    if (item.kind == AssetKind::Sample && ext.empty())
        ext = fs::u8path(item.fileName).extension().string();
    const std::string file = (folder / fs::u8path(stem + ext)).string();
    const std::string unpacked = (folder / fs::u8path(stem)).string();

    auto progress = job->progress;
    auto unpacking = job->unpacking;
    auto alive = alive_;
    env::Environment &env = env_;
    const bool archive = item.kind != AssetKind::Sample;
    const std::string url = item.url;
    env::runJob(env_, env::Lane::Network, [this, alive, &env, job, progress, unpacking, archive, url, file, unpacked] {
        std::string error;
        engine::HttpRequest request;
        request.url = url;
        request.progress = progress.get();
        bool ok = engine::httpDownload(request, file, error);
        if (ok && archive) {
            env.post([this, alive, job] {
                if (*alive && job->state == State::Downloading)
                    job->state = State::Unpacking;
            });
            ok = extractArchive(file, unpacked, error, unpacking.get());
            std::error_code ec;
            fs::remove(fs::u8path(file), ec); // the archive is done with either way
            if (!ok)
                fs::remove_all(fs::u8path(unpacked), ec);
        }
        const bool cancelled = progress->cancel;
        env.post([this, alive, job, ok, cancelled, error, file, unpacked] {
            if (!*alive)
                return;
            if (!ok) {
                job->state = cancelled ? State::Cancelled : State::Failed;
                job->error = cancelled ? std::string() : error;
            } else {
                install(*job, file, unpacked);
            }
            auto callback = callbacks_.find(job);
            if (callback != callbacks_.end()) {
                auto fn = std::move(callback->second);
                callbacks_.erase(callback);
                fn(*job);
            }
        });
    }, work_);
}

void Downloads::install(Job &job, const std::string &file, const std::string &unpacked)
{
    job.state = State::Installing;
    const OnlineItem &item = job.item;
    std::string error;
    std::error_code ec;
    // Where it came from and under what licence, beside it
    std::ostringstream note;
    note << item.name << "\n";
    if (!item.author.empty())
        note << "By " << item.author << "\n";
    if (!item.license.empty())
        note << "Licence: " << item.license << "\n";
    if (!item.page.empty())
        note << "From " << item.page << "\n";
    note << "Downloaded with Vibin from " << item.source << ".\n";

    if (item.kind == AssetKind::Sample) {
        const std::string name = fs::u8path(item.fileName).stem().string();
        job.installed = library_.installSamples(file, name.empty() ? item.name : name, item.category, error);
        if (!job.installed.empty())
            writeFile(fs::u8path(job.installed).replace_extension(".txt"), note.str());
    } else {
        // An archive with a single folder at its top (GitHub's) is that folder
        fs::path root = fs::u8path(unpacked);
        for (int depth = 0; depth < 3; ++depth) {
            std::vector<fs::path> children;
            for (const auto &e : fs::directory_iterator(root, ec))
                children.push_back(e.path());
            if (children.size() == 1 && fs::is_directory(children.front(), ec))
                root = children.front();
            else
                break;
        }
        if (item.kind == AssetKind::Instrument && audio::findSfz(root.string()).empty()) {
            // No SFZ: note-named files will do, else it isn't an instrument this can play
            bool notes = false;
            for (const auto &e : fs::directory_iterator(root, ec)) {
                if (audio::keyFromName(e.path().stem().string()) >= 0)
                    notes = true;
            }
            if (!notes)
                error = "It has no .sfz file or note-named samples to play";
        }
        if (error.empty()) {
            writeFile(root / "Vibin source.txt", note.str());
            job.installed = item.kind == AssetKind::Instrument
                                ? library_.installInstrument(root.string(), item.name, item.category, error)
                                : library_.installSamples(root.string(), item.name, item.category, error);
        }
        fs::remove_all(fs::u8path(unpacked), ec);
    }
    if (job.installed.empty()) {
        job.state = State::Failed;
        job.error = error.empty() ? "Couldn't put it in the library" : error;
        fs::remove(fs::u8path(file), ec);
        return;
    }
    job.state = State::Done;
}

}
