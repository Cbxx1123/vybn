#include "Manager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <set>
#include <thread>

#include "env/Environment.h"

namespace plugin {

namespace fs = std::filesystem;

namespace {

std::string variable(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Fields are tab-separated; tabs, line ends and backslashes inside them are escaped
std::string escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        if (c == '\\')
            out += "\\\\";
        else if (c == '\t')
            out += "\\t";
        else if (c == '\n')
            out += "\\n";
        else if (c == '\r')
            out += "\\r";
        else
            out += c;
    }
    return out;
}

std::vector<std::string> fields(const std::string &line)
{
    std::vector<std::string> out(1);
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\t') {
            out.emplace_back();
        } else if (c == '\\' && i + 1 < line.size()) {
            const char n = line[++i];
            out.back() += n == 't' ? '\t' : n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            out.back() += c;
        }
    }
    return out;
}

bool fileStamp(const std::string &path, uint64_t &size, int64_t &modified)
{
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    size = fs::file_size(p, ec);
    if (ec)
        return false;
    modified = static_cast<int64_t>(fs::last_write_time(p, ec).time_since_epoch().count());
    return !ec;
}

}

std::vector<ScanRecord> readScanCache(const std::string &file)
{
    std::vector<ScanRecord> out;
    std::ifstream in(fs::u8path(file), std::ios::binary);
    std::string line;
    if (!std::getline(in, line) || line.rfind("# Vibin plugin scans 1", 0) != 0)
        return out;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::vector<std::string> f = fields(line);
        if (f[0] == "file" && f.size() >= 5) {
            ScanRecord r;
            r.path = f[1];
            r.size = std::strtoull(f[2].c_str(), nullptr, 10);
            r.modified = std::strtoll(f[3].c_str(), nullptr, 10);
            r.error = f[4];
            out.push_back(std::move(r));
        } else if (f[0] == "plugin" && f.size() >= 8 && !out.empty()) {
            Descriptor d;
            d.id = f[1];
            d.name = f[2];
            d.vendor = f[3];
            d.version = f[4];
            d.description = f[5];
            d.url = f[6];
            d.features = f[7];
            d.path = out.back().path;
            if (!d.id.empty())
                out.back().plugins.push_back(std::move(d));
        }
    }
    return out;
}

bool writeScanCache(const std::string &file, const std::vector<ScanRecord> &records)
{
    std::error_code ec;
    const fs::path path = fs::u8path(file);
    fs::create_directories(path.parent_path(), ec);
    const fs::path temp = fs::u8path(file + ".saving");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << "# Vibin plugin scans 1: what each .clap held when it was last scanned\n";
        for (const ScanRecord &r : records) {
            out << "file\t" << escape(r.path) << "\t" << r.size << "\t" << r.modified << "\t" << escape(r.error) << "\n";
            for (const Descriptor &d : r.plugins)
                out << "plugin\t" << escape(d.id) << "\t" << escape(d.name) << "\t" << escape(d.vendor) << "\t" << escape(d.version)
                    << "\t" << escape(d.description) << "\t" << escape(d.url) << "\t" << escape(d.features) << "\n";
        }
        if (!out)
            return false;
    }
    fs::rename(temp, path, ec);
    return !ec;
}

PluginManager::PluginManager(env::Environment &env, PluginConfig config) : Daemon("Plugins"), env_(env), config_(std::move(config)) {}

PluginManager::~PluginManager() { jobs_.wait(); }

std::vector<std::string> PluginManager::standardFolders()
{
    std::vector<std::string> out;
    const std::string common = variable("COMMONPROGRAMFILES");
    if (!common.empty())
        out.push_back(common + "\\CLAP");
    const std::string local = variable("LOCALAPPDATA");
    if (!local.empty())
        out.push_back(local + "\\Programs\\Common\\CLAP");
    return out;
}

void PluginManager::setConfig(PluginConfig config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(config);
}

std::vector<std::string> PluginManager::findClapFiles(const std::string &folder)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(fs::u8path(folder), ec))
        return out;
    fs::recursive_directory_iterator it(fs::u8path(folder), fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 2) {
            it.disable_recursion_pending();
            continue;
        }
        const fs::path &p = it->path();
        if (lower(p.extension().u8string()) == ".clap" && it->is_regular_file(ec))
            out.push_back(fs::absolute(p, ec).lexically_normal().u8string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

SandboxOptions PluginManager::sandboxOptions(const std::string &id) const
{
    SandboxOptions options;
    std::lock_guard<std::mutex> lock(mutex_);
    options.appContainer = config_.sandbox;
    auto it = config_.grants.find(id);
    if (it != config_.grants.end())
        options.readable = it->second;
    auto w = config_.writeGrants.find(id);
    if (w != config_.writeGrants.end())
        options.writable = w->second;
    return options;
}

bool PluginManager::enabled(const std::string &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::find(config_.disabled.begin(), config_.disabled.end(), id) == config_.disabled.end();
}

void PluginManager::rescan(bool force)
{
    if (scanning_.exchange(true))
        return;
    std::vector<std::string> folders;
    std::string cacheFile;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        folders = config_.folders;
        cacheFile = config_.cacheFile;
    }
    const SandboxOptions options = sandboxOptions();
    env::runJob(env_, env::Lane::Background,
                [this, folders, options, cacheFile, force] {
                    // What earlier scans found, by path (not when forced)
                    std::map<std::string, ScanRecord> cache;
                    if (!force && !cacheFile.empty())
                        for (ScanRecord &r : readScanCache(cacheFile))
                            cache[lower(r.path)] = std::move(r);
                    std::vector<ScanRecord> records;
                    ScanStats stats;
                    std::vector<Descriptor> found;
                    std::vector<ScanProblem> problems;
                    std::set<std::string> seenFiles, seenIds;
                    for (const std::string &folder : folders) {
                        for (const std::string &file : findClapFiles(folder)) {
                            if (!seenFiles.insert(lower(file)).second)
                                continue;
                            ++stats.files;
                            ScanRecord record;
                            record.path = file;
                            fileStamp(file, record.size, record.modified);
                            auto hit = cache.find(lower(file));
                            if (hit != cache.end() && hit->second.size == record.size && hit->second.modified == record.modified) {
                                record.error = hit->second.error;
                                record.plugins = hit->second.plugins;
                                for (Descriptor &d : record.plugins)
                                    d.path = file;
                                ++stats.cached;
                            } else {
                                std::string error;
                                if (!Instance::scan(file, options, record.plugins, error))
                                    record.error = error.empty() ? "it couldn't be scanned" : error;
                                ++stats.scanned;
                            }
                            if (!record.error.empty())
                                problems.push_back({file, record.error});
                            // The first file to offer an id wins
                            for (const Descriptor &d : record.plugins)
                                if (seenIds.insert(d.id).second)
                                    found.push_back(d);
                            records.push_back(std::move(record));
                        }
                    }
                    if (!cacheFile.empty() && (stats.scanned > 0 || records.size() != cache.size()))
                        writeScanCache(cacheFile, records);
                    std::sort(found.begin(), found.end(), [](const Descriptor &a, const Descriptor &b) { return lower(a.name) < lower(b.name); });
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        plugins_ = std::move(found);
                        problems_ = std::move(problems);
                        stats_ = stats;
                    }
                    ++version_;
                    scanning_ = false;
                },
                jobs_);
}

ScanStats PluginManager::lastScan() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

bool PluginManager::waitForScan(uint32_t ms) const
{
    for (uint32_t waited = 0; scanning_.load() && waited < ms; waited += 10)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return !scanning_.load();
}

std::vector<Descriptor> PluginManager::plugins() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return plugins_;
}

std::vector<ScanProblem> PluginManager::problems() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return problems_;
}

bool PluginManager::find(const std::string &id, Descriptor &out) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Descriptor &d : plugins_)
        if (d.id == id) {
            out = d;
            return true;
        }
    return false;
}

std::shared_ptr<Instance> PluginManager::create(const std::string &id, std::string &error)
{
    Descriptor d;
    if (!find(id, d)) {
        error = "No plugin \"" + id + "\" is installed";
        return nullptr;
    }
    return create(d, error);
}

std::shared_ptr<Instance> PluginManager::create(const Descriptor &plugin, std::string &error)
{
    if (!enabled(plugin.id)) {
        error = plugin.name + " is turned off in Settings > Plugins";
        return nullptr;
    }
    std::shared_ptr<Instance> instance(Instance::create(plugin, sandboxOptions(plugin.id), error));
    if (instance) {
        std::lock_guard<std::mutex> lock(liveMutex_);
        live_.push_back(instance);
    }
    return instance;
}

void PluginManager::tick(env::Environment &)
{
    std::vector<std::shared_ptr<Instance>> live;
    {
        std::lock_guard<std::mutex> lock(liveMutex_);
        live_.erase(std::remove_if(live_.begin(), live_.end(), [](const std::weak_ptr<Instance> &w) { return w.expired(); }), live_.end());
        for (const auto &w : live_)
            if (auto s = w.lock())
                live.push_back(std::move(s));
    }
    for (const auto &instance : live)
        instance->poll();
}

void PluginManager::stop(env::Environment &) { jobs_.wait(); }

}
