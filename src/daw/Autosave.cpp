#include "Autosave.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <cwctype>

#include "ProjectFile.h"
#include "ProjectSession.h"
#include "Toml.h"
#include "env/Environment.h"
#include "env/Scheduler.h"

#ifdef _WIN32
// QueryFullProcessImageNameW is Windows Vista's; MinGW's headers default to an older one
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#endif

namespace daw {

namespace fs = std::filesystem;

namespace {

std::string environment(const char *name)
{
    const char *value = std::getenv(name);
    return value ? value : "";
}

std::string readText(const fs::path &path, bool &ok)
{
    std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Writes text to `path` through `path`.part, replacing what was there
bool writeReplacing(const fs::path &path, const std::string &text, std::string &error)
{
    fs::path part = path;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Couldn't write " + part.string();
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            error = "Couldn't write " + part.string();
            std::error_code ec;
            fs::remove(part, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::remove(path, ec); // rename won't replace on every platform
    fs::rename(part, path, ec);
    if (ec) {
        error = "Couldn't replace " + path.string() + ": " + ec.message();
        fs::remove(part, ec);
        return false;
    }
    return true;
}

// A file's last write time as seconds since the Unix epoch
double fileSeconds(fs::file_time_type time)
{
    const auto system = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(time - fs::file_time_type::clock::now());
    return std::chrono::duration<double>(system.time_since_epoch()).count();
}
bool hasVersionFile(const AutosaveManifest &manifest, const std::string &file)
{
    return std::any_of(manifest.versions.begin(), manifest.versions.end(),
                       [&file](const AutosaveVersion &v) { return v.file == file; });
}

}

long long currentProcessId()
{
#ifdef _WIN32
    return static_cast<long long>(GetCurrentProcessId());
#else
    return 0;
#endif
}

std::string autosaveRoot()
{
    const std::string override = environment("VIBIN_AUTOSAVE_ROOT");
    if (!override.empty())
        return override;
    std::string base = environment("LOCALAPPDATA");
    if (base.empty())
        base = (fs::path(environment("HOME")) / ".cache").string();
    return (fs::path(base) / "Vibin" / "Autosave").string();
}

double unixSeconds()
{
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string autosaveKey(const std::string &path)
{
    // Case-blind and slash-blind, so the same file names the same folder however it was typed
    std::string normal;
    for (char c : path) {
        if (c == '\\')
            c = '/';
        normal += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (normal.size() > 1 && normal.back() == '/')
        normal.pop_back();
    uint64_t hash = 1469598103934665603ULL; // FNV-1a, 64 bits
    for (unsigned char c : normal) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(hash));
    return buffer;
}

std::string untitledKey(double started, unsigned long pid)
{
    return "untitled-" + std::to_string(static_cast<long long>(started)) + "-" + std::to_string(pid);
}

std::string versionFileName(double time)
{
    const std::time_t seconds = static_cast<std::time_t>(std::floor(time));
    const std::tm *local = std::localtime(&seconds);
    char buffer[64];
    if (!local || std::strftime(buffer, sizeof(buffer), "autosave-%Y%m%d-%H%M%S.vbp", local) == 0)
        return "autosave-00000000-000000.vbp";
    return buffer;
}

std::string writeManifest(const AutosaveManifest &manifest)
{
    std::string out = "# Vibin autosave versions\n";
    out += "format = 1\n";
    out += "path = " + toml::quote(manifest.path) + "\n";
    out += "name = " + toml::quote(manifest.name) + "\n";
    out += "updated = " + toml::number(manifest.updated) + "\n";
    for (const AutosaveVersion &v : manifest.versions) {
        out += "\n[[versions]]\n";
        out += "file = " + toml::quote(v.file) + "\n";
        out += "time = " + toml::number(v.time) + "\n";
        out += "kind = " + toml::quote(v.kind) + "\n";
        out += "label = " + toml::quote(v.label) + "\n";
    }
    return out;
}

bool parseManifest(const std::string &text, AutosaveManifest &manifest, std::string &error)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return false;
    if (root.get("format", static_cast<int64_t>(0)) != 1) {
        error = "an autosave manifest of another format";
        return false;
    }
    AutosaveManifest read;
    read.path = root.get("path", "");
    read.name = root.get("name", "");
    read.updated = root.get("updated", 0.0);
    if (const toml::Array *versions = root.arrayAt("versions")) {
        for (const toml::Value &item : *versions) {
            if (!item.isTable())
                continue;
            AutosaveVersion v;
            v.file = item.get("file", "");
            v.time = item.get("time", 0.0);
            v.kind = item.get("kind", "auto");
            v.label = item.get("label", "");
            if (!v.file.empty())
                read.versions.push_back(v);
        }
    }
    manifest = std::move(read);
    return true;
}

AutosaveManifest readManifest(const std::string &folder)
{
    AutosaveManifest manifest;
    bool ok = false;
    const std::string text = readText(fs::path(folder) / "manifest.toml", ok);
    std::string error;
    if (ok && parseManifest(text, manifest, error))
        return manifest;
    return AutosaveManifest{};
}

bool deleteVersion(const std::string &folder, const std::string &file, std::string &error)
{
    AutosaveManifest manifest = readManifest(folder);
    manifest.versions.erase(std::remove_if(manifest.versions.begin(), manifest.versions.end(),
                                           [&file](const AutosaveVersion &v) { return v.file == file; }),
                            manifest.versions.end());
    // The manifest stops naming the version before its file goes
    if (!writeReplacing(fs::path(folder) / "manifest.toml", writeManifest(manifest), error))
        return false;
    std::error_code ec;
    fs::remove(fs::path(folder) / file, ec);
    return true;
}

std::string autosaveTimeText(double time, double now)
{
    const std::time_t t = static_cast<std::time_t>(std::floor(time));
    const std::time_t n = static_cast<std::time_t>(std::floor(now));
    std::tm when{}, today{};
#ifdef _WIN32
    localtime_s(&when, &t);
    localtime_s(&today, &n);
#else
    localtime_r(&t, &when);
    localtime_r(&n, &today);
#endif
    char clock[16];
    std::strftime(clock, sizeof(clock), "%H:%M", &when);
    if (when.tm_year == today.tm_year && when.tm_yday == today.tm_yday)
        return std::string("today ") + clock;
    if (when.tm_year == today.tm_year && when.tm_yday + 1 == today.tm_yday)
        return std::string("yesterday ") + clock;
    char day[32];
    if (when.tm_year == today.tm_year)
        std::strftime(day, sizeof(day), "%d %b", &when);
    else
        std::strftime(day, sizeof(day), "%d %b %Y", &when);
    std::string text = day;
    if (text.size() > 1 && text[0] == '0')
        text.erase(0, 1); // "8 Oct", not "08 Oct"
    return text + " " + clock;
}

std::vector<AutosaveVersion> rotate(std::vector<AutosaveVersion> &versions, int keep)
{
    std::vector<size_t> automatic;
    for (size_t i = 0; i < versions.size(); ++i) {
        if (versions[i].kind != "manual")
            automatic.push_back(i);
    }
    std::vector<AutosaveVersion> removed;
    const size_t limit = static_cast<size_t>(std::max(keep, 0));
    if (automatic.size() <= limit)
        return removed;
    // The oldest automatic ones go
    std::stable_sort(automatic.begin(), automatic.end(),
                     [&versions](size_t a, size_t b) { return versions[a].time < versions[b].time; });
    std::vector<bool> drop(versions.size(), false);
    for (size_t k = 0; k < automatic.size() - limit; ++k)
        drop[automatic[k]] = true;
    std::vector<AutosaveVersion> kept;
    for (size_t i = 0; i < versions.size(); ++i) {
        if (drop[i])
            removed.push_back(versions[i]);
        else
            kept.push_back(versions[i]);
    }
    versions = std::move(kept);
    return removed;
}

std::string writeSessionLock(const SessionLock &lock)
{
    std::string out = "# Vibin session lock: the session that is running\n";
    out += "pid = " + std::to_string(lock.pid) + "\n";
    out += "started = " + toml::number(lock.started) + "\n";
    out += "key = " + toml::quote(lock.key) + "\n";
    out += "path = " + toml::quote(lock.path) + "\n";
    return out;
}

bool parseSessionLock(const std::string &text, SessionLock &lock, std::string &error)
{
    toml::Value root;
    if (!toml::parse(text, root, error))
        return false;
    SessionLock read;
    read.pid = root.get("pid", static_cast<int64_t>(0));
    read.started = root.get("started", 0.0);
    read.key = root.get("key", "");
    read.path = root.get("path", "");
    if (read.pid <= 0) {
        error = "a session lock without a process";
        return false;
    }
    lock = std::move(read);
    return true;
}

bool isRecoverable(const SessionLock &lock, double now, bool running, double newestAutosave, double projectFileTime)
{
    if (lock.pid <= 0 || running || newestAutosave <= 0.0)
        return false;
    // An autosave stamped well after now can't be from this machine's clock; leave it alone
    if (newestAutosave > now + 60.0)
        return false;
    // A saved project whose file is at least as new as its newest autosave was saved after it
    if (projectFileTime > 0.0 && newestAutosave <= projectFileTime)
        return false;
    return true;
}

bool processRunning(long long pid)
{
#ifdef _WIN32
    if (pid <= 0)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!process)
        // Access refused means a process that exists but isn't ours to look at: assume it runs
        return GetLastError() == ERROR_ACCESS_DENIED;
    wchar_t path[MAX_PATH * 4];
    DWORD size = sizeof(path) / sizeof(path[0]);
    bool running = false;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        std::wstring name(path, size);
        const size_t slash = name.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            name = name.substr(slash + 1);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        running = name == L"main.exe" || name == L"main-dev.exe";
    }
    CloseHandle(process);
    return running;
#else
    (void)pid;
    return false;
#endif
}

Recoverable findRecoverable(const std::string &root, double now)
{
    Recoverable found;
    SessionLock lock;
    if (!readSessionLockFile(root, lock))
        return found;
    const bool running = processRunning(lock.pid);

    // The newest automatic version in the session's folder
    AutosaveManifest manifest;
    const fs::path folder = fs::path(root) / lock.key;
    bool ok = false;
    const std::string text = readText(folder / "manifest.toml", ok);
    std::string error;
    if (!ok || !parseManifest(text, manifest, error))
        manifest = AutosaveManifest{};
    const AutosaveVersion *newest = nullptr;
    for (const AutosaveVersion &v : manifest.versions) {
        if (v.kind == "manual")
            continue;
        if (!newest || v.time > newest->time)
            newest = &v;
    }

    // A saved project's file: its last write, when it is there
    double projectTime = 0.0;
    std::error_code ec;
    if (!lock.path.empty() && fs::exists(lock.path, ec))
        projectTime = fileSeconds(fs::last_write_time(lock.path, ec));

    const double newestTime = newest ? newest->time : 0.0;
    if (!isRecoverable(lock, now, running, newestTime, projectTime))
        return found;
    // Declined before, and nothing newer since
    if (recoveryDeclined(readDeclined(folder.string()), newestTime))
        return found;
    found.found = true;
    found.key = lock.key;
    found.pid = lock.pid;
    found.name = manifest.name;
    found.path = lock.path;
    found.autosave = (folder / newest->file).string();
    found.autosaveTime = newest->time;
    found.started = lock.started;
    return found;
}

bool recoveryDeclined(double declinedAt, double newestAutosave)
{
    return declinedAt > 0.0 && newestAutosave <= declinedAt;
}

void writeDeclined(const std::string &folder, double newestAutosave, std::string &error)
{
    std::error_code ec;
    fs::create_directories(folder, ec);
    writeReplacing(fs::path(folder) / "declined.txt", toml::number(newestAutosave) + "\n", error);
}

double readDeclined(const std::string &folder)
{
    bool ok = false;
    const std::string text = readText(fs::path(folder) / "declined.txt", ok);
    if (!ok)
        return 0.0;
    return std::strtod(text.c_str(), nullptr);
}

bool writeAutosaveVersion(const std::string &folder, const std::string &path, const Project &project, double time,
                          bool manual, const std::string &label, int keep, std::string &file, std::string &error)
{
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "Couldn't make " + folder + ": " + ec.message();
        return false;
    }

    AutosaveManifest manifest;
    const fs::path manifestPath = fs::path(folder) / "manifest.toml";
    bool exists = false;
    const std::string previous = readText(manifestPath, exists);
    if (exists) {
        std::string parseError;
        if (!parseManifest(previous, manifest, parseError))
            manifest = AutosaveManifest{}; // an unreadable manifest starts again; its files stay unlisted
    }

    // A version's name comes from its time; a second that is taken moves on by a second
    double stamp = std::floor(time);
    std::string name = versionFileName(stamp);
    while (hasVersionFile(manifest, name) || fs::exists(fs::path(folder) / name, ec)) {
        stamp += 1.0;
        name = versionFileName(stamp);
    }

    // Sound files are written with their full paths: the version sits in the autosave folder, not beside
    // the project, so paths relative to the project would not resolve when it is read back
    const std::string text = writeProject(project);
    if (!writeReplacing(fs::path(folder) / name, text, error))
        return false;

    manifest.path = path;
    manifest.name = project.name;
    manifest.updated = stamp;
    AutosaveVersion version;
    version.file = name;
    version.time = stamp;
    version.kind = manual ? "manual" : "auto";
    version.label = label;
    manifest.versions.push_back(version);
    const std::vector<AutosaveVersion> removed = rotate(manifest.versions, keep);

    // The manifest names the new version before the old files go, so it never names a missing one
    if (!writeReplacing(manifestPath, writeManifest(manifest), error)) {
        fs::remove(fs::path(folder) / name, ec);
        return false;
    }
    for (const AutosaveVersion &old : removed)
        fs::remove(fs::path(folder) / old.file, ec);

    file = name;
    return true;
}

bool writeSessionLockFile(const std::string &root, const SessionLock &lock, std::string &error)
{
    std::error_code ec;
    fs::create_directories(root, ec);
    return writeReplacing(fs::path(root) / "session.lock", writeSessionLock(lock), error);
}

bool readSessionLockFile(const std::string &root, SessionLock &lock)
{
    bool ok = false;
    const std::string text = readText(fs::path(root) / "session.lock", ok);
    if (!ok)
        return false;
    std::string error;
    return parseSessionLock(text, lock, error);
}

void removeSessionLockFile(const std::string &root, long long pid)
{
    SessionLock lock;
    if (!readSessionLockFile(root, lock) || lock.pid != pid)
        return;
    std::error_code ec;
    fs::remove(fs::path(root) / "session.lock", ec);
}

// One write: the job's result, picked up on the main thread once finished
struct AutosaveController::Done {
    std::atomic<bool> finished{false};
    bool ok = false;
    std::string file;
    std::string error;
};

AutosaveController::AutosaveController(env::Environment *env, std::string root, Clock now)
    : env_(env), root_(std::move(root)), now_(std::move(now))
{
    started_ = now_();
    sessionKey_ = untitledKey(started_, static_cast<unsigned long>(currentProcessId()));
}

AutosaveController::~AutosaveController()
{
    group_.wait(); // a write under way finishes (or is dropped by a stopped scheduler)
    if (lockWritten_)
        removeSessionLockFile(root_, static_cast<long long>(currentProcessId()));
}

void AutosaveController::configure(bool enabled, double intervalSeconds, int keep)
{
    enabled_ = enabled;
    interval_ = std::max(1.0, intervalSeconds);
    keep_ = std::clamp(keep, 1, 50);
}

std::string AutosaveController::keyFor(const ProjectSession &session) const
{
    return session.path().empty() ? sessionKey_ : autosaveKey(session.path());
}

std::string AutosaveController::folderFor(const ProjectSession &session) const
{
    return (fs::path(root_) / keyFor(session)).string();
}

void AutosaveController::writeLock(const ProjectSession &session)
{
    SessionLock lock;
    lock.pid = currentProcessId();
    lock.started = started_;
    lock.key = keyFor(session);
    lock.path = session.path();
    std::string error;
    writeSessionLockFile(root_, lock, error); // a small file; a failure only loses the crash check
    lockWritten_ = true;
    lockedPath_ = session.path();
    lockedKey_ = lock.key;
}

AutosaveController::Event AutosaveController::update(const ProjectSession &session, bool settled, std::string &error)
{
    Event event = Event::None;
    // A write that finished since the last look
    if (running_ && running_->finished.load()) {
        if (running_->ok) {
            event = runningManual_ ? Event::VersionSaved : Event::Autosaved;
        } else {
            event = Event::Failed;
            error = running_->error;
        }
        running_.reset();
    }

    // The session lock follows the project's file, so a crash finds the right folder
    if (!lockWritten_ || session.path() != lockedPath_ || keyFor(session) != lockedKey_)
        writeLock(session);

    // Unsaved changes: the time they were first seen, and the revision the last copy was taken at
    const uint64_t revision = session.store().revision();
    const double t = now_();
    if (revision == savedRevision_ || !session.modified())
        firstChange_ = -1.0;
    else if (firstChange_ < 0.0)
        firstChange_ = t;

    const bool due = enabled_ && settled && session.modified() && revision != savedRevision_ && firstChange_ >= 0.0 &&
                     std::max(lastAutosave_, firstChange_) + interval_ <= t;
    if (due && !busy()) {
        Request request;
        request.project = session.project();
        request.folder = (fs::path(root_) / keyFor(session)).string();
        request.path = session.path();
        request.time = std::floor(t);
        request.keep = keep_;
        queue_.push_back(std::move(request));
        savedRevision_ = revision;
        firstChange_ = -1.0;
        lastAutosave_ = t;
    }
    pump();
    return event;
}

void AutosaveController::saveVersion(const ProjectSession &session, const std::string &label)
{
    Request request;
    request.project = session.project();
    request.folder = (fs::path(root_) / keyFor(session)).string();
    request.path = session.path();
    request.manual = true;
    request.label = label;
    request.time = std::floor(now_());
    request.keep = keep_;
    queue_.push_back(std::move(request));
    pump();
}

void AutosaveController::pump()
{
    if (running_ || queue_.empty())
        return;
    Request request = std::move(queue_.front());
    queue_.pop_front();
    start(std::move(request));
}

void AutosaveController::start(Request request)
{
    auto done = std::make_shared<Done>();
    running_ = done;
    runningManual_ = request.manual;
    auto shared = std::make_shared<Request>(std::move(request));
    const int keep = shared->keep;
    auto job = [shared, done, keep] {
        done->ok = writeAutosaveVersion(shared->folder, shared->path, shared->project, shared->time, shared->manual,
                                        shared->label, keep, done->file, done->error);
        done->finished = true;
    };
    if (env_)
        env::runJob(*env_, env::Lane::Background, job, group_);
    else
        job();
}

}
