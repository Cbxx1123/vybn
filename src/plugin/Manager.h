#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Instance.h"
#include "env/Daemon.h"
#include "env/Scheduler.h"

namespace env {
class Environment;
}

namespace plugin {

struct PluginConfig {
    // Folders searched for plugins (and their subfolders): .clap files, .vst3 bundles and files; the
    // standard CLAP and VST3 folders and the user's own
    std::vector<std::string> folders;
    bool sandbox = true; // run hosts in the AppContainer
    std::vector<std::string> disabled; // plugin ids turned off: listed, but never made
    // Folders a plugin may read besides its own file (its sample content), by plugin id, and
    // folders it may write in too (its presets, its database)
    std::map<std::string, std::vector<std::string>> grants;
    std::map<std::string, std::vector<std::string>> writeGrants;
    // Where scans are remembered (empty: nowhere). A plugin file whose size and time haven't changed
    // since it was scanned isn't loaded again; one that failed is remembered as failing.
    std::string cacheFile;
};

// What the last scan did
struct ScanStats {
    size_t files = 0;   // plugin files found
    size_t scanned = 0; // scanned again (in a host, or a VST3 bundle from its moduleinfo.json)
    size_t cached = 0;  // taken from the cache
    size_t probed = 0;  // effect plugins whose sidechain port was asked after the scan (each in a host of its own)
};

// A scanned file, as the cache keeps it
struct ScanRecord {
    std::string path;
    uint64_t size = 0;
    int64_t modified = 0; // the file's time, as the file system counts it
    std::string error;    // why it couldn't be scanned (empty when it could)
    std::vector<Descriptor> plugins;
};
// The cache file: read (missing or unreadable: empty) and written (beside, then renamed)
std::vector<ScanRecord> readScanCache(const std::string &file);
bool writeScanCache(const std::string &file, const std::vector<ScanRecord> &records);

// A plugin file that couldn't be scanned, and why
struct ScanProblem {
    std::string path;
    std::string error;
};

// The program's plugins: the catalogue of what the plugin folders hold, and every live instance,
// whose watchdogs it runs. A daemon added after the Scheduler. Scanning loads each plugin file in a
// sandboxed host of its own (a plugin can crash just by being loaded) as a Background job.
class PluginManager : public env::Daemon {
public:
    PluginManager(env::Environment &env, PluginConfig config = {});
    ~PluginManager() override;

    // The standard plugin folders on this system: %COMMONPROGRAMFILES%\CLAP and \VST3, and
    // %LOCALAPPDATA%\Programs\Common\CLAP and \VST3
    static std::vector<std::string> standardFolders();

    PluginConfig config() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return config_;
    }
    void setConfig(PluginConfig config);

    // Scans every folder again in the background (main thread): files unchanged since the cache
    // remembered them aren't loaded again, unless `force` (Settings > Plugins' Scan again)
    void rescan(bool force = false);
    ScanStats lastScan() const;
    bool scanning() const { return scanning_.load(); }
    // Waits (on a job's thread) while a scan is under way, up to `ms`; whether it has finished
    bool waitForScan(uint32_t ms) const;
    // Up once a scan has finished
    uint64_t version() const { return version_.load(); }
    // The catalogue and the problems from the last scan (main thread). A plugin installed as both
    // CLAP and VST3 (the same name and vendor) is listed once, as CLAP; the other is still found by
    // its id, for projects that use it
    std::vector<Descriptor> plugins() const;
    std::vector<ScanProblem> problems() const;
    bool find(const std::string &id, Descriptor &out) const;
    // A plugin's sidechain input channels (1 or 2) as the probe after the scan found them, 0 for none, an
    // instrument, an unknown id or a plugin whose probe failed; no copy, so it is cheap to ask every frame
    uint32_t sidechainChannels(const std::string &id) const;

    // Makes an instance of plugin `id` in a host of its own. Blocks while the host starts and
    // loads it, so call it from a job. The manager watches it until the last holder lets go.
    std::shared_ptr<Instance> create(const std::string &id, std::string &error);
    std::shared_ptr<Instance> create(const Descriptor &plugin, std::string &error);
    SandboxOptions sandboxOptions(const std::string &id = {}) const;
    bool enabled(const std::string &id) const;

    // Finds the plugin files under a folder (and its subfolders, two deep): .clap files, .vst3
    // bundles (whose insides aren't searched) and single .vst3 files
    static std::vector<std::string> findPluginFiles(const std::string &folder);

    // For slots: where their jobs run, and the group the manager waits for when it stops
    env::Environment &environment() { return env_; }
    env::JobGroup &jobs() { return jobs_; }

    void tick(env::Environment &env) override;
    void stop(env::Environment &env) override;

private:
    env::Environment &env_;
    PluginConfig config_;
    mutable std::mutex mutex_;
    std::vector<Descriptor> plugins_;
    std::vector<Descriptor> others_; // the formats of listed plugins that aren't listed
    std::vector<ScanProblem> problems_;
    ScanStats stats_;
    std::atomic<bool> scanning_{false};
    std::atomic<uint64_t> version_{0};
    env::JobGroup jobs_;

    std::mutex liveMutex_;
    std::vector<std::weak_ptr<Instance>> live_;
};

}
