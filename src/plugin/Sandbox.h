#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plugin {

// How a plugin host process is contained. Native plugin code can't be contained inside the app's
// own process, so each plugin instance runs in vybn-plugin-host.exe, and this is the fence
// around it:
// - A job object: the host dies when the app closes it (or the app dies), may not start other
//   processes, is capped in memory, ends at once on a crash (no error box), and can't touch the
//   desktop's settings, global atoms or exit Windows.
// - An AppContainer with no capabilities ("VYBN.PluginSandbox"): no network, and no access to the
//   user's files, registry or other processes except what is granted. The host's executable and
//   each plugin's .clap file (and folders the user grants it) are granted read and execute, by
//   adding the container to their access lists. The system's own files stay readable.
// When Windows refuses the AppContainer the host runs in the job object alone, and says so.
struct SandboxOptions {
    bool appContainer = true;
    // Files and folders the plugin may read: its .clap and any folders granted to it (folders
    // with everything in them)
    std::vector<std::string> readable;
    // Folders it may also write in (its presets, its database): read, write and delete
    std::vector<std::string> writable;
    uint64_t memoryLimit = 4ull << 30; // bytes the host may commit
};

// A sandboxed host process. Not copyable; terminating on destruction.
class HostProcess {
public:
    HostProcess() = default;
    ~HostProcess();
    HostProcess(const HostProcess &) = delete;
    HostProcess &operator=(const HostProcess &) = delete;

    // Starts `exe` with `arguments`, passing it only `inherit` (inheritable handles)
    bool launch(const std::string &exe, const std::string &arguments, const std::vector<void *> &inherit,
                const SandboxOptions &options, std::string &error);

    bool running() const;
    // The exit code once it has ended
    uint32_t exitCode() const;
    // Ends it at once
    void terminate();
    // Waits up to `ms` for it to end; whether it did
    bool wait(uint32_t ms) const;

    // Whether it runs in the AppContainer, and if not why not
    bool contained() const { return contained_; }
    // The process handle (signalled once it has ended), for waiting on
    void *handle() const { return process_; }
    const std::string &note() const { return note_; }

private:
    void *process_ = nullptr;
    void *job_ = nullptr;
    bool contained_ = false;
    std::string note_;
};

// vybn-plugin-host.exe beside the running program
std::string hostExecutable();
// A Windows exit code as words ("an access violation", "exit code 3")
std::string describeExit(uint32_t code);

}
