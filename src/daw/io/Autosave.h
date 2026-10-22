#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "daw/model/Project.h"
#include "env/Scheduler.h"

namespace env {
class Environment;
}

namespace daw {

class ProjectSession;

// Autosave: while a project has unsaved changes, a copy of it is written every so often into the
// autosave folder, so a crash loses little. Each project (or each unsaved one, per session) has a
// folder of its versions, a manifest.toml describing them, and the app keeps a session.lock that
// says which session is running, so the next start can offer to recover what a crash left.
// Everything here is in plain files; the writes run on a background job, never the main thread.

// %LOCALAPPDATA%\Vibin\Autosave, or the folder named by VIBIN_AUTOSAVE_ROOT (tests and demos use it
// so the user's own folder is never touched)
std::string autosaveRoot();

// The current time in seconds since the Unix epoch
double unixSeconds();

// The folder key of a saved project: 16 hex digits of a hash of its normalised path
std::string autosaveKey(const std::string &path);
// The folder key of an unsaved session: "untitled-<start time>-<process id>"
std::string untitledKey(double started, unsigned long pid);
// A version's file name from its time, in local time: autosave-YYYYMMDD-HHMMSS.vbp
std::string versionFileName(double time);

// One saved version of a project: its file in the folder, when it was made, "auto" or "manual",
// and the label a manual one was given
struct AutosaveVersion {
    std::string file;
    double time = 0.0;
    std::string kind = "auto";
    std::string label;
};

// The manifest.toml of a folder: the project's path (empty when unsaved), its name, when the
// folder was last written, and its versions oldest first
struct AutosaveManifest {
    std::string path;
    std::string name;
    double updated = 0.0;
    std::vector<AutosaveVersion> versions;
};

std::string writeManifest(const AutosaveManifest &manifest);
bool parseManifest(const std::string &text, AutosaveManifest &manifest, std::string &error);
// A folder's manifest (empty when there is none), and the removal of one version: its file and its
// entry in the manifest (the file is kept when the entry can't be written)
AutosaveManifest readManifest(const std::string &folder);
bool deleteVersion(const std::string &folder, const std::string &file, std::string &error);

// A version's time as a list shows it, against `now`: "today 14:32", "yesterday 14:32", "18 Oct 09:05",
// or "18 Oct 2025 09:05" for another year
std::string autosaveTimeText(double time, double now);

// Rotation: keeps the newest `keep` automatic versions and every manual one, removes the older
// automatic ones from the list and returns them (so the caller can delete their files)
std::vector<AutosaveVersion> rotate(std::vector<AutosaveVersion> &versions, int keep);

// The session lock (session.lock in the autosave root): the process that is running the app, when
// it started, the autosave folder key it writes to, and the project's path (empty when unsaved)
struct SessionLock {
    long long pid = 0;
    double started = 0.0;
    std::string key;
    std::string path;
};

std::string writeSessionLock(const SessionLock &lock);
bool parseSessionLock(const std::string &text, SessionLock &lock, std::string &error);

// Whether a lock left behind by a session that has ended can be recovered: its process is no
// longer a running Vibin, and its folder has an automatic version newer than the project's file
// (or any, for a project never saved, which has no file: projectFileTime is 0). `newestAutosave`
// is 0 when there is none.
bool isRecoverable(const SessionLock &lock, double now, bool processRunning, double newestAutosave,
                   double projectFileTime);

// This process's id (the session lock names it)
long long currentProcessId();

// Whether a process is a running copy of the app (main.exe or main-dev.exe). Windows only; elsewhere
// it says no.
bool processRunning(long long pid);

// Looks at the autosave root for a session that ended without closing. `found` is set only when
// isRecoverable says so, and then the fields describe the session's newest automatic version.
struct Recoverable {
    bool found = false;
    std::string key;        // the folder under the autosave root
    long long pid = 0;      // the process the session lock names (it has ended)
    std::string name;       // the project's name
    std::string path;       // the project's file, empty when it was never saved
    std::string autosave;   // the newest automatic version's full path
    double autosaveTime = 0.0;
    double started = 0.0;   // when that session started
};

Recoverable findRecoverable(const std::string &root, double now);

// Recovery declined ("Not now"): the newest autosave's time is kept in the session's folder, and that
// session is offered again only once an autosave newer than it exists
bool recoveryDeclined(double declinedAt, double newestAutosave);
void writeDeclined(const std::string &folder, double newestAutosave, std::string &error);
double readDeclined(const std::string &folder); // 0 when none

// Writes a version of a project into a folder: the project as writeProject writes it (sound files
// relative to the project's folder, `path`'s), to a .part file that is then renamed, the manifest
// updated and rotated, and the rotated files removed. `file` is the version's name on success.
bool writeAutosaveVersion(const std::string &folder, const std::string &path, const Project &project, double time,
                          bool manual, const std::string &label, int keep, std::string &file, std::string &error);

// The session lock, written and read in the autosave root, and removed when `pid` still owns it
bool writeSessionLockFile(const std::string &root, const SessionLock &lock, std::string &error);
bool readSessionLockFile(const std::string &root, SessionLock &lock);
void removeSessionLockFile(const std::string &root, long long pid);

// Autosave on the main thread's clock: the project is copied there, and writes run as Background
// jobs one at a time. Call update once a frame, with the same `settled` the undo history uses (no
// mouse held, no text being typed). Without an environment (tests) the writes run on the calling
// thread instead.
class AutosaveController {
public:
    using Clock = std::function<double()>;

    // What update() reports: a finished autosave, a finished manual version, or a failed write
    enum class Event { None, Autosaved, VersionSaved, Failed };

    AutosaveController(env::Environment *env, std::string root, Clock now);
    // Waits for a write under way, then removes the session lock (a clean exit)
    ~AutosaveController();

    AutosaveController(const AutosaveController &) = delete;
    AutosaveController &operator=(const AutosaveController &) = delete;

    // Whether autosaving runs, how long to wait after the last one (or the first change), and how
    // many automatic versions to keep
    void configure(bool enabled, double intervalSeconds, int keep);

    // Once a frame, on the main thread. Writes the session lock when the project's path changes,
    // and starts an autosave when the project has unsaved changes that are settled and the interval
    // has passed. `error` is set when the event is Failed.
    Event update(const ProjectSession &session, bool settled, std::string &error);

    // Writes a manual version now (it waits behind a write under way; never rotated)
    void saveVersion(const ProjectSession &session, const std::string &label);

    // The folder key the session writes to (its path's key once saved, else the session's own)
    std::string keyFor(const ProjectSession &session) const;
    // The folder the session's versions are in (under the root)
    std::string folderFor(const ProjectSession &session) const;

    bool busy() const { return running_ != nullptr || !queue_.empty(); }
    const std::string &root() const { return root_; }

private:
    struct Request {
        Project project;
        std::string folder;
        std::string path;
        bool manual = false;
        std::string label;
        double time = 0.0;
        int keep = 10;
    };
    struct Done;

    void pump();
    void start(Request request);
    void writeLock(const ProjectSession &session);

    env::Environment *env_;
    std::string root_;
    Clock now_;
    bool enabled_ = true;
    double interval_ = 120.0;
    int keep_ = 10;

    double started_ = 0.0;
    std::string sessionKey_;     // the folder of this session while unsaved
    bool lockWritten_ = false;
    std::string lockedPath_;
    std::string lockedKey_;

    uint64_t savedRevision_ = 0; // the revision the last copy was taken at
    double firstChange_ = -1.0;  // when unsaved changes were first seen (-1 for none)
    double lastAutosave_ = -1.0; // when the last automatic copy was taken (-1 for none)

    std::deque<Request> queue_;
    std::shared_ptr<Done> running_;
    bool runningManual_ = false;
    env::JobGroup group_;
};

}
