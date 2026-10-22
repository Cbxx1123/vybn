// vibin-icon-patch.exe: gives the app's executable the icon of the user's theme, once the app has ended.
//
// The icon inside an executable is data in the file, so it can't change while the program runs (the file
// is locked) and can't be drawn by the program itself afterwards. So when the app ends with a theme whose
// icon differs from the executable's (AppIcon.cpp: scheduleExecutableIcon), it starts this, which waits
// for the app to be gone, then replaces the icon:
//
//   vibin-icon-patch <process id> <the executable> <a .ico file>
//
// The icon's images are stored as plain bitmaps (AppIcon.cpp: encodeDib), which for a given size in pixels
// are always the same number of bytes, whatever the colours. So the new images go over the old ones,
// byte for byte, where the executable's resource section holds them, and nothing else in the file moves
// or changes: its code, its debug information, its size. (Windows' own UpdateResource rewrites the whole
// file and drops what it doesn't know: a debug build lost 24 MB of symbols to it.)
//
// Every image is checked first, the executable's against the new icon's, by size in pixels and by size in
// bytes, and nothing is written unless all of them match. The .ico file is deleted when this ends.
//
// A plain Windows program with no console (-mwindows), static, so it needs nothing the app's folder
// doesn't have.

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <string>
#include <vector>

namespace {

uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p) { return le16(p) | (static_cast<uint32_t>(le16(p + 2)) << 16); }

bool readFile(const std::wstring &path, std::vector<uint8_t> &out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size();
    }
    CloseHandle(file);
    return ok;
}

// The images of a .ico file, by their width in pixels (0 in an entry stands for 256)
bool parseIcon(const std::vector<uint8_t> &file, std::map<int, std::vector<uint8_t>> &images)
{
    if (file.size() < 6 || le16(&file[0]) != 0 || le16(&file[2]) != 1)
        return false;
    const size_t count = le16(&file[4]);
    if (count == 0 || count > 32 || file.size() < 6 + 16 * count)
        return false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *e = &file[6 + 16 * i];
        const uint32_t size = le32(e + 8), offset = le32(e + 12);
        if (static_cast<uint64_t>(offset) + size > file.size() || size == 0)
            return false;
        images[e[0] == 0 ? 256 : e[0]].assign(file.begin() + offset, file.begin() + offset + size);
    }
    return true;
}

bool readAt(HANDLE file, uint64_t offset, void *into, DWORD bytes)
{
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD got = 0;
    return SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && ReadFile(file, into, bytes, &got, nullptr) && got == bytes;
}

bool writeAt(HANDLE file, uint64_t offset, const void *from, DWORD bytes)
{
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(offset);
    DWORD put = 0;
    return SetFilePointerEx(file, at, nullptr, FILE_BEGIN) && WriteFile(file, from, bytes, &put, nullptr) && put == bytes;
}

// The executable's resource section, read whole (a few megabytes at most: the rest of the file isn't touched)
struct Resources {
    std::vector<uint8_t> bytes;
    uint32_t sectionRva = 0;  // where the section is in memory
    uint64_t sectionFile = 0; // and where in the file
    uint32_t rootOffset = 0;  // the resource directory, from the start of `bytes`
};

bool loadResources(HANDLE exe, Resources &out)
{
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(exe, &size) || size.QuadPart < 1024)
        return false;
    std::vector<uint8_t> head(static_cast<size_t>((std::min<LONGLONG>)(size.QuadPart, 8192)));
    if (!readAt(exe, 0, head.data(), static_cast<DWORD>(head.size())))
        return false;
    if (head.size() < sizeof(IMAGE_DOS_HEADER) || le16(&head[0]) != IMAGE_DOS_SIGNATURE)
        return false;
    const size_t nt = le32(&head[0x3C]); // e_lfanew
    if (nt + 24 + 128 > head.size() || le32(&head[nt]) != IMAGE_NT_SIGNATURE)
        return false;
    const uint16_t sections = le16(&head[nt + 6]);
    const uint16_t optionalSize = le16(&head[nt + 20]);
    const size_t optional = nt + 24;
    const uint16_t magic = le16(&head[optional]);
    const size_t directories = optional + (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC ? 112 : 96);
    if (directories + 24 > head.size())
        return false;
    const uint32_t resourceRva = le32(&head[directories + IMAGE_DIRECTORY_ENTRY_RESOURCE * 8]);
    if (resourceRva == 0)
        return false;
    const size_t table = optional + optionalSize;
    for (uint16_t i = 0; i < sections && table + 40 * (i + 1) <= head.size(); ++i) {
        const uint8_t *s = &head[table + 40 * i];
        const uint32_t virtualSize = le32(s + 8), va = le32(s + 12), rawSize = le32(s + 16), raw = le32(s + 20);
        if (resourceRva >= va && resourceRva < va + (std::max)(virtualSize, rawSize)) {
            if (rawSize == 0 || rawSize > (64u << 20) || static_cast<uint64_t>(raw) + rawSize > static_cast<uint64_t>(size.QuadPart))
                return false;
            out.bytes.resize(rawSize);
            out.sectionRva = va;
            out.sectionFile = raw;
            out.rootOffset = resourceRva - va;
            return readAt(exe, raw, out.bytes.data(), rawSize);
        }
    }
    return false;
}

// One level of the resource tree: the entry in the directory at `dir` (from the root) with this id or
// this name; its offset (from the root) and whether it is another directory
bool findEntry(const Resources &r, uint32_t dir, bool byId, uint32_t id, const wchar_t *name, uint32_t &offset, bool &isDirectory)
{
    const size_t at = r.rootOffset + dir;
    if (at + 16 > r.bytes.size())
        return false;
    const size_t named = le16(&r.bytes[at + 12]), ids = le16(&r.bytes[at + 14]);
    for (size_t i = 0; i < named + ids; ++i) {
        const size_t e = at + 16 + 8 * i;
        if (e + 8 > r.bytes.size())
            return false;
        const uint32_t key = le32(&r.bytes[e]), target = le32(&r.bytes[e + 4]);
        bool match = false;
        if (byId) {
            match = i >= named && key == id;
        } else if (i < named && (key & 0x80000000u)) {
            const size_t s = r.rootOffset + (key & 0x7FFFFFFFu);
            if (s + 2 <= r.bytes.size()) {
                const size_t length = le16(&r.bytes[s]);
                match = s + 2 + 2 * length <= r.bytes.size() && length == std::wcslen(name);
                for (size_t k = 0; match && k < length; ++k)
                    match = std::towupper(le16(&r.bytes[s + 2 + 2 * k])) == std::towupper(name[k]);
            }
        }
        if (match) {
            isDirectory = (target & 0x80000000u) != 0;
            offset = target & 0x7FFFFFFFu;
            return true;
        }
    }
    return false;
}

// The data of a resource (type, then name or id, then its first language): where it is in the section
// buffer and how long
bool resourceData(const Resources &r, uint32_t type, bool byId, uint32_t id, const wchar_t *name, size_t &at, uint32_t &size)
{
    uint32_t typeDir = 0, nameDir = 0;
    bool isDir = false;
    if (!findEntry(r, 0, true, type, nullptr, typeDir, isDir) || !isDir)
        return false;
    if (!findEntry(r, typeDir, byId, id, name, nameDir, isDir) || !isDir)
        return false;
    // The first language: the first entry of the name's directory (named ones come first, then ids)
    const size_t dir = r.rootOffset + nameDir;
    if (dir + 24 > r.bytes.size() || le16(&r.bytes[dir + 12]) + le16(&r.bytes[dir + 14]) == 0)
        return false;
    const uint32_t languageEntry = le32(&r.bytes[dir + 16 + 4]);
    if (languageEntry & 0x80000000u)
        return false;
    const size_t entry = r.rootOffset + languageEntry;
    if (entry + 16 > r.bytes.size())
        return false;
    const uint32_t rva = le32(&r.bytes[entry]);
    size = le32(&r.bytes[entry + 4]);
    if (rva < r.sectionRva || static_cast<uint64_t>(rva - r.sectionRva) + size > r.bytes.size())
        return false;
    at = rva - r.sectionRva;
    return true;
}

struct Write {
    size_t at;
    const std::vector<uint8_t> *data;
};

// Every image of the icon group (named by id or name) in the executable, matched with the new icon's image of
// the same width in pixels; false when one is missing or not the same number of bytes
bool planGroup(const Resources &r, bool byId, uint32_t id, const wchar_t *name, const std::map<int, std::vector<uint8_t>> &images,
               std::vector<Write> &plan)
{
    size_t at = 0;
    uint32_t size = 0;
    if (!resourceData(r, 14, byId, id, name, at, size) || size < 6)
        return false;
    const size_t count = le16(&r.bytes[at + 4]);
    if (count == 0 || 6 + 14 * count > size)
        return false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *entry = &r.bytes[at + 6 + 14 * i];
        const int width = entry[0] == 0 ? 256 : entry[0];
        const uint32_t iconId = le16(entry + 12);
        const auto found = images.find(width);
        size_t imageAt = 0;
        uint32_t imageSize = 0;
        if (found == images.end() || !resourceData(r, 3, true, iconId, nullptr, imageAt, imageSize) || imageSize != found->second.size())
            return false;
        plan.push_back({imageAt, &found->second});
    }
    return true;
}

bool patch(const std::wstring &exePath, const std::map<int, std::vector<uint8_t>> &images)
{
    // The program may be a moment from released (its end, a scan of it), so opening it is tried for a while
    HANDLE exe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 40 && exe == INVALID_HANDLE_VALUE; ++attempt) {
        exe = CreateFileW(exePath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
        if (exe == INVALID_HANDLE_VALUE)
            Sleep(250);
    }
    if (exe == INVALID_HANDLE_VALUE)
        return false;

    Resources resources;
    std::vector<Write> plan;
    // The two icon groups the app's resource file has (src/win/vibin.rc.in), each with its images
    bool ok = loadResources(exe, resources) && planGroup(resources, true, 1, nullptr, images, plan) &&
              planGroup(resources, false, 0, L"GLFW_ICON", images, plan);
    for (size_t i = 0; ok && i < plan.size(); ++i)
        ok = writeAt(exe, resources.sectionFile + plan[i].at, plan[i].data->data(), static_cast<DWORD>(plan[i].data->size()));
    FlushFileBuffers(exe);
    // What is in the file now is what was meant
    for (size_t i = 0; ok && i < plan.size(); ++i) {
        std::vector<uint8_t> back(plan[i].data->size());
        ok = readAt(exe, resources.sectionFile + plan[i].at, back.data(), static_cast<DWORD>(back.size())) && back == *plan[i].data;
    }
    CloseHandle(exe);
    return ok;
}

}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 4)
        return 2;
    const DWORD pid = static_cast<DWORD>(std::wcstoul(argv[1], nullptr, 10));
    const std::wstring target = argv[2], icon = argv[3];
    LocalFree(argv);

    // Until the program that started this has ended (its file is locked while it runs)
    if (pid != 0) {
        if (HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
            WaitForSingleObject(process, 120000);
            CloseHandle(process);
        }
    }

    std::vector<uint8_t> file;
    std::map<int, std::vector<uint8_t>> images;
    const bool ok = readFile(icon, file) && parseIcon(file, images) && patch(target, images);
    DeleteFileW(icon.c_str());
    if (ok) {
        // Explorer, the desktop and the taskbar are told the icon changed: the file, its folder, and (what an
        // installer does when it changes an icon) that associations did, which has the shell drop the icons
        // it holds. A window that was already open may still show the old one until it is opened again.
        std::wstring folder = target.substr(0, target.find_last_of(L"\\/"));
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSH, target.c_str(), nullptr);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSH, folder.c_str(), nullptr);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
    }
    return ok ? 0 : 1;
}
