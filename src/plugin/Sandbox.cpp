// Windows 10 APIs (AppContainers, process attributes); MinGW's headers otherwise stop at XP
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "Sandbox.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <userenv.h>
#endif

#include <cstdio>
#include <mutex>
#include <set>

namespace plugin {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

const wchar_t kContainerName[] = L"VYBN.PluginSandbox";

// The container's SID, derived once from its name. Its profile (a registry key and an empty data
// folder under %LOCALAPPDATA%\Packages) is made only when Windows asks for it.
std::mutex containerMutex;
PSID containerSid = nullptr;
bool containerTried = false;
std::string containerError;

PSID container(std::string &error)
{
    std::lock_guard<std::mutex> lock(containerMutex);
    if (!containerTried) {
        containerTried = true;
        PSID sid = nullptr;
        const HRESULT hr = DeriveAppContainerSidFromAppContainerName(kContainerName, &sid);
        if (SUCCEEDED(hr))
            containerSid = sid;
        else {
            char text[64];
            std::snprintf(text, sizeof text, "0x%08lx", static_cast<unsigned long>(hr));
            containerError = std::string("Windows refused the plugin sandbox (") + text + ")";
        }
    }
    error = containerError;
    return containerSid;
}

// Makes the container's profile if it isn't there yet; whether it is now
bool ensureProfile()
{
    std::lock_guard<std::mutex> lock(containerMutex);
    PSID sid = nullptr;
    const HRESULT hr = CreateAppContainerProfile(kContainerName, L"VYBN plugins", L"CLAP plugins hosted by the VYBN DAW", nullptr, 0, &sid);
    if (sid)
        FreeSid(sid);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

// Paths already granted this run, so each is changed once
std::mutex grantMutex;
std::set<std::wstring> granted;

// Whether `acl` already allows `sid` read and execute (and write, with `write`)
bool allows(PACL acl, PSID sid, bool folder, bool write)
{
    if (!acl)
        return false;
    for (DWORD i = 0; i < acl->AceCount; ++i) {
        void *ace = nullptr;
        if (!GetAce(acl, i, &ace))
            continue;
        const ACE_HEADER *header = static_cast<ACE_HEADER *>(ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
            continue;
        const ACCESS_ALLOWED_ACE *allowed = static_cast<ACCESS_ALLOWED_ACE *>(ace);
        if (!EqualSid(const_cast<DWORD *>(&allowed->SidStart), sid))
            continue;
        const DWORD needed = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
        bool rights = (allowed->Mask & needed) == needed || (allowed->Mask & GENERIC_ALL) ||
                      ((allowed->Mask & GENERIC_READ) && (allowed->Mask & GENERIC_EXECUTE));
        if (write)
            rights = rights && ((allowed->Mask & GENERIC_ALL) || (allowed->Mask & GENERIC_WRITE) ||
                                (allowed->Mask & FILE_GENERIC_WRITE) == FILE_GENERIC_WRITE);
        const bool inherits = !folder || (header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) == (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
        if (rights && inherits)
            return true;
    }
    return false;
}

// Lets the container read (and run) a file, or a folder and everything in it, and with `write`
// change and delete what is in it too
bool grantAccess(PSID sid, const std::wstring &path, bool write, std::string &error)
{
    const std::wstring key = (write ? L"w:" : L"r:") + path;
    {
        std::lock_guard<std::mutex> lock(grantMutex);
        if (granted.count(key))
            return true;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = "Can't find " + narrow(path);
        return false;
    }
    const bool folder = attributes & FILE_ATTRIBUTE_DIRECTORY;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) !=
        ERROR_SUCCESS) {
        error = "Can't read the permissions of " + narrow(path);
        return false;
    }
    bool ok = true;
    if (!allows(acl, sid, folder, write)) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE | (write ? GENERIC_WRITE | DELETE : 0);
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = folder ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        PACL updated = nullptr;
        if (SetEntriesInAclW(1, &access, acl, &updated) != ERROR_SUCCESS ||
            SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated,
                                  nullptr) != ERROR_SUCCESS) {
            error = std::string("Can't let the sandbox ") + (write ? "write in " : "read ") + narrow(path);
            ok = false;
        }
        if (updated)
            LocalFree(updated);
    }
    LocalFree(descriptor);
    if (ok) {
        std::lock_guard<std::mutex> lock(grantMutex);
        granted.insert(key);
    }
    return ok;
}

HANDLE makeJob(uint64_t memoryLimit)
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION |
                                              JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memoryLimit);
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS | JOB_OBJECT_UILIMIT_EXITWINDOWS |
                             JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
    SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof ui);
    return job;
}

}

HostProcess::~HostProcess()
{
    terminate();
    if (process_)
        CloseHandle(static_cast<HANDLE>(process_));
    if (job_)
        CloseHandle(static_cast<HANDLE>(job_)); // kills anything left in it
}

bool HostProcess::launch(const std::string &exe, const std::string &arguments, const std::vector<void *> &inherit,
                         const SandboxOptions &options, std::string &error)
{
    const std::wstring exeW = widen(exe);
    if (GetFileAttributesW(exeW.c_str()) == INVALID_FILE_ATTRIBUTES) {
        error = "The plugin host is missing (" + exe + ")";
        return false;
    }
    HANDLE job = makeJob(options.memoryLimit);
    if (!job) {
        error = "Couldn't make a job object for the plugin";
        return false;
    }
    job_ = job;

    // The container, and what it may read
    PSID sid = nullptr;
    if (options.appContainer) {
        std::string why;
        sid = container(why);
        if (sid) {
            if (!grantAccess(sid, exeW, false, why))
                sid = nullptr;
            for (size_t i = 0; sid && i < options.readable.size(); ++i)
                if (!grantAccess(sid, widen(options.readable[i]), false, why))
                    sid = nullptr;
            for (size_t i = 0; sid && i < options.writable.size(); ++i)
                if (!grantAccess(sid, widen(options.writable[i]), true, why))
                    sid = nullptr;
        }
        if (!sid)
            note_ = "Not sandboxed: " + why;
    } else {
        note_ = "Not sandboxed: the sandbox is turned off for this plugin";
    }

    std::vector<HANDLE> handles;
    for (void *h : inherit)
        handles.push_back(static_cast<HANDLE>(h));

    auto start = [&](PSID containerSid, std::string &why) -> bool {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
        std::vector<char> storage(size);
        auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 2, 0, &size)) {
            why = "Couldn't prepare the plugin host's start";
            return false;
        }
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(), handles.size() * sizeof(HANDLE),
                                  nullptr, nullptr);
        SECURITY_CAPABILITIES capabilities{};
        if (containerSid) {
            capabilities.AppContainerSid = containerSid;
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, &capabilities, sizeof capabilities,
                                      nullptr, nullptr);
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof startup;
        startup.lpAttributeList = attributes;
        std::wstring command = L"\"" + exeW + L"\" " + widen(arguments);
        // The system folder as its working folder: always readable, and nothing of the user's
        wchar_t system[MAX_PATH] = {};
        GetSystemDirectoryW(system, MAX_PATH);
        PROCESS_INFORMATION info{};
        const BOOL made = CreateProcessW(exeW.c_str(), &command[0], nullptr, nullptr, TRUE,
                                         EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, system,
                                         &startup.StartupInfo, &info);
        DeleteProcThreadAttributeList(attributes);
        if (!made) {
            why = "Couldn't start the plugin host (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!AssignProcessToJobObject(job, info.hProcess)) {
            TerminateProcess(info.hProcess, 1);
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
            why = "Couldn't put the plugin host in its job object";
            return false;
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process_ = info.hProcess;
        return true;
    };

    std::string why;
    if (sid && start(sid, why)) {
        contained_ = true;
        return true;
    }
    // Windows wants the container's profile (its registry key and data folder) before it starts
    // anything in it: make it once, and try again
    if (sid && ensureProfile()) {
        why.clear();
        if (start(sid, why)) {
            contained_ = true;
            return true;
        }
    }
    if (sid)
        note_ = "Not sandboxed: " + why;
    if (!start(nullptr, error))
        return false;
    return true;
}

bool HostProcess::running() const { return process_ && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT; }

uint32_t HostProcess::exitCode() const
{
    DWORD code = 0;
    if (process_)
        GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    return code;
}

void HostProcess::terminate()
{
    if (running())
        TerminateProcess(static_cast<HANDLE>(process_), 0xDEAD);
}

bool HostProcess::wait(uint32_t ms) const
{
    return !process_ || WaitForSingleObject(static_cast<HANDLE>(process_), ms) != WAIT_TIMEOUT;
}

std::string hostExecutable()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring w(path, n);
    const size_t slash = w.find_last_of(L"\\/");
    return narrow(w.substr(0, slash + 1) + L"vybn-plugin-host.exe");
}

std::string describeExit(uint32_t code)
{
    switch (code) {
    case 0xC0000005:
        return "an access violation";
    case 0xC00000FD:
        return "a stack overflow";
    case 0xC0000017:
    case 0xC000012D:
        return "running out of memory";
    case 0xC0000094:
        return "a division by zero";
    case 0xC0000409:
        return "a security check failing";
    case 0xC000001D:
        return "an illegal instruction";
    case 0xDEAD:
        return "being stopped by VYBN";
    default: {
        char text[48];
        std::snprintf(text, sizeof text, code >= 0xC0000000 ? "error 0x%08X" : "exit code %u", code);
        return text;
    }
    }
}

#else

HostProcess::~HostProcess() {}
bool HostProcess::launch(const std::string &, const std::string &, const std::vector<void *> &, const SandboxOptions &, std::string &error)
{
    error = "Plugins are only hosted on Windows for now";
    return false;
}
bool HostProcess::running() const { return false; }
uint32_t HostProcess::exitCode() const { return 0; }
void HostProcess::terminate() {}
bool HostProcess::wait(uint32_t) const { return true; }
std::string hostExecutable() { return "vybn-plugin-host"; }
std::string describeExit(uint32_t code) { return "exit code " + std::to_string(code); }

#endif

}
