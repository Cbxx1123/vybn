#include "FileDialog.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace engine {

#ifdef _WIN32

namespace {

// Paths go through the system code page, like the rest of the program's file access
std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The filter string: label\0patterns\0 ... \0
std::wstring filterString(const std::vector<FileFilter> &filters)
{
    std::wstring filter;
    for (const FileFilter &f : filters) {
        filter += widen(f.label) + L'\0' + widen(f.patterns) + L'\0';
    }
    filter += L'\0';
    return filter;
}

// The dialog wants backslashes in its starting folder
std::wstring windowsPath(const std::string &path)
{
    std::wstring w = widen(path);
    for (wchar_t &c : w) {
        if (c == L'/')
            c = L'\\';
    }
    return w;
}

}

std::vector<std::string> openFileDialog(const std::string &title, const std::vector<FileFilter> &filters, bool multiple,
                                        const std::string &folder)
{
    const std::wstring filter = filterString(filters);
    std::wstring buffer(64 * 1024, L'\0');
    const std::wstring caption = widen(title);
    const std::wstring start = windowsPath(folder);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filters.empty() ? nullptr : filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = caption.c_str();
    ofn.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (multiple)
        ofn.Flags |= OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn))
        return {};

    // One file: its full path. Several: the folder, then each name, each ending in \0.
    std::vector<std::wstring> parts;
    const wchar_t *p = buffer.c_str();
    while (*p) {
        parts.emplace_back(p);
        p += parts.back().size() + 1;
    }
    std::vector<std::string> paths;
    if (parts.size() == 1) {
        paths.push_back(narrow(parts[0]));
    } else {
        for (size_t i = 1; i < parts.size(); ++i)
            paths.push_back(narrow(parts[0] + L"\\" + parts[i]));
    }
    return paths;
}

std::string saveFileDialog(const std::string &title, const std::vector<FileFilter> &filters, const std::string &folder,
                           const std::string &name, const std::string &extension)
{
    const std::wstring filter = filterString(filters);
    std::wstring buffer(32 * 1024, L'\0');
    // Characters Windows refuses in file names would make the dialog reject the suggestion
    std::wstring suggested = widen(name);
    const std::wstring refused = L"<>:\"/\\|?*";
    for (wchar_t &c : suggested) {
        if (refused.find(c) != std::wstring::npos || c < 32)
            c = L'_';
    }
    suggested = suggested.substr(0, buffer.size() - 1);
    suggested.copy(buffer.data(), suggested.size());
    const std::wstring caption = widen(title);
    const std::wstring start = windowsPath(folder);
    // The default extension, without its dot
    const std::wstring ext = widen(!extension.empty() && extension[0] == '.' ? extension.substr(1) : extension);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filters.empty() ? nullptr : filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = caption.c_str();
    ofn.lpstrInitialDir = start.empty() ? nullptr : start.c_str();
    ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn))
        return {};
    return narrow(buffer.c_str());
}

bool listFolder(const std::string &path, std::vector<FolderEntry> &out, std::string &error)
{
    out.clear();
    std::wstring pattern = windowsPath(path);
    if (!pattern.empty() && pattern.back() != L'\\')
        pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW data;
    // The basic info (no short names) in large fetches: much quicker on big and network folders
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                   FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND)
            return true; // an empty drive
        error = code == ERROR_ACCESS_DENIED ? "You don't have permission to open this folder."
                : code == ERROR_PATH_NOT_FOUND ? "This folder doesn't exist."
                : code == ERROR_NOT_READY ? "The drive isn't ready."
                : "This folder can't be read (error " + std::to_string(code) + ").";
        return false;
    }
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        FolderEntry e;
        e.name = narrow(name);
        e.folder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!e.folder)
            e.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        // FILETIME counts 100 ns steps from 1601
        const uint64_t t = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
        e.modified = static_cast<int64_t>(t / 10000000ull) - 11644473600ll;
        out.push_back(std::move(e));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
}

std::string knownFolder(KnownFolder which)
{
    int csidl = -1;
    switch (which) {
    case KnownFolder::Home:
        csidl = CSIDL_PROFILE;
        break;
    case KnownFolder::Documents:
        csidl = CSIDL_PERSONAL;
        break;
    case KnownFolder::Desktop:
        csidl = CSIDL_DESKTOPDIRECTORY;
        break;
    case KnownFolder::Music:
        csidl = CSIDL_MYMUSIC;
        break;
    case KnownFolder::Downloads:
        break; // no CSIDL: the usual place in the profile
    }
    if (csidl >= 0) {
        wchar_t buffer[MAX_PATH];
        if (SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buffer) == S_OK)
            return narrow(buffer);
        return {};
    }
    const std::string home = knownFolder(KnownFolder::Home);
    if (home.empty())
        return {};
    std::error_code ec;
    const std::filesystem::path downloads = std::filesystem::path(home) / "Downloads";
    return std::filesystem::is_directory(downloads, ec) ? downloads.string() : std::string();
}

std::vector<DriveInfo> drives()
{
    std::vector<DriveInfo> list;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i)))
            continue;
        const std::string root = std::string(1, static_cast<char>('A' + i)) + ":\\";
        const std::string letter = root.substr(0, 2);
        const char *kind = "Drive";
        switch (GetDriveTypeA(root.c_str())) {
        case DRIVE_FIXED:
            kind = "Local disk";
            break;
        case DRIVE_REMOVABLE:
            kind = "Removable";
            break;
        case DRIVE_REMOTE:
            kind = "Network";
            break;
        case DRIVE_CDROM:
            kind = "CD drive";
            break;
        case DRIVE_RAMDISK:
            kind = "RAM disk";
            break;
        default:
            break;
        }
        list.push_back({root, std::string(kind) + " (" + letter + ")"});
    }
    return list;
}

#else

bool listFolder(const std::string &path, std::vector<FolderEntry> &out, std::string &error)
{
    out.clear();
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.')
            continue;
        FolderEntry e;
        e.name = name;
        e.folder = it->is_directory(ec);
        if (!e.folder)
            e.size = it->file_size(ec);
        out.push_back(std::move(e));
    }
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

std::string knownFolder(KnownFolder which)
{
    const char *home = std::getenv("HOME");
    if (!home)
        return {};
    switch (which) {
    case KnownFolder::Home:
        return home;
    case KnownFolder::Documents:
        return std::string(home) + "/Documents";
    case KnownFolder::Desktop:
        return std::string(home) + "/Desktop";
    case KnownFolder::Downloads:
        return std::string(home) + "/Downloads";
    case KnownFolder::Music:
        return std::string(home) + "/Music";
    }
    return {};
}

std::vector<DriveInfo> drives()
{
    return {{"/", "Computer"}};
}

std::vector<std::string> openFileDialog(const std::string &, const std::vector<FileFilter> &, bool, const std::string &)
{
    return {};
}

std::string saveFileDialog(const std::string &, const std::vector<FileFilter> &, const std::string &,
                           const std::string &, const std::string &)
{
    return {};
}

#endif

}
