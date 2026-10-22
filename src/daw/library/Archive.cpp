#include "daw/library/Archive.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "engine/Shell.h"

// stb_image's inflate (its zlib decoder), without the image decoders
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include <stb_image.h>

namespace daw {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool endsWith(const std::string &s, const std::string &tail)
{
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

uint16_t u16(const unsigned char *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const unsigned char *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t u64(const unsigned char *p) { return u32(p) | (static_cast<uint64_t>(u32(p + 4)) << 32); }

struct Entry {
    std::string name;
    uint16_t method = 0;
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t compressed = 0;
    uint64_t size = 0;
    uint64_t offset = 0; // of the local header
};

// A path inside the archive as one under `folder`, or empty when it would escape it
fs::path safePath(const fs::path &folder, std::string name)
{
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos)
        return {};
    fs::path out = folder;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos)
            end = name.size();
        const std::string part = name.substr(start, end - start);
        if (part == "..")
            return {};
        if (!part.empty() && part != ".")
            out /= fs::u8path(part);
        start = end + 1;
    }
    return out;
}

bool readAt(std::ifstream &in, uint64_t offset, void *data, size_t size)
{
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    in.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount()) == size;
}

}

uint32_t crc32(const void *data, size_t size, uint32_t crc)
{
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    const auto *p = static_cast<const unsigned char *>(data);
    crc = ~crc;
    for (size_t i = 0; i < size; ++i)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool isArchive(const std::string &name)
{
    const std::string n = lower(name);
    for (const char *ext : {".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tar.xz", ".7z"}) {
        if (endsWith(n, ext))
            return true;
    }
    return false;
}

bool extractZip(const std::string &archive, const std::string &folder, std::string &error, ExtractProgress *progress)
{
    std::ifstream in(fs::u8path(archive), std::ios::binary);
    if (!in) {
        error = "Can't open " + archive;
        return false;
    }
    in.seekg(0, std::ios::end);
    const uint64_t fileSize = static_cast<uint64_t>(in.tellg());

    // The end of central directory record: within the last 64 KB + 22 bytes (it may have a comment)
    const uint64_t tail = std::min<uint64_t>(fileSize, 65557);
    std::vector<unsigned char> end(static_cast<size_t>(tail));
    if (tail < 22 || !readAt(in, fileSize - tail, end.data(), end.size())) {
        error = "Not a zip file";
        return false;
    }
    size_t eocd = std::string::npos;
    for (size_t i = end.size() - 22 + 1; i-- > 0;) {
        if (u32(&end[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd == std::string::npos) {
        error = "Not a zip file (or it was cut short)";
        return false;
    }
    uint64_t count = u16(&end[eocd + 10]);
    uint64_t dirSize = u32(&end[eocd + 12]);
    uint64_t dirOffset = u32(&end[eocd + 16]);
    // Zip64: a locator just before points at the 64-bit record
    if (eocd >= 20 && u32(&end[eocd - 20]) == 0x07064b50) {
        const uint64_t at = u64(&end[eocd - 20 + 8]);
        unsigned char rec[56];
        if (readAt(in, at, rec, sizeof(rec)) && u32(rec) == 0x06064b50) {
            count = u64(rec + 32);
            dirSize = u64(rec + 40);
            dirOffset = u64(rec + 48);
        }
    }
    if (dirOffset + dirSize > fileSize || dirSize > (1ull << 31)) {
        error = "The zip file's directory is damaged";
        return false;
    }

    std::vector<unsigned char> dir(static_cast<size_t>(dirSize));
    if (!readAt(in, dirOffset, dir.data(), dir.size())) {
        error = "Can't read the zip file's directory";
        return false;
    }
    std::vector<Entry> entries;
    size_t p = 0;
    for (uint64_t n = 0; n < count; ++n) {
        if (p + 46 > dir.size() || u32(&dir[p]) != 0x02014b50) {
            error = "The zip file's directory is damaged";
            return false;
        }
        Entry e;
        e.flags = u16(&dir[p + 8]);
        e.method = u16(&dir[p + 10]);
        e.crc = u32(&dir[p + 16]);
        e.compressed = u32(&dir[p + 20]);
        e.size = u32(&dir[p + 24]);
        const size_t nameLen = u16(&dir[p + 28]);
        const size_t extraLen = u16(&dir[p + 30]);
        const size_t commentLen = u16(&dir[p + 32]);
        e.offset = u32(&dir[p + 42]);
        if (p + 46 + nameLen + extraLen + commentLen > dir.size()) {
            error = "The zip file's directory is damaged";
            return false;
        }
        e.name.assign(reinterpret_cast<const char *>(&dir[p + 46]), nameLen);
        // Zip64 sizes and offset, in that order, for the fields that overflowed
        size_t x = p + 46 + nameLen;
        const size_t xEnd = x + extraLen;
        while (x + 4 <= xEnd) {
            const uint16_t id = u16(&dir[x]);
            const uint16_t len = u16(&dir[x + 2]);
            if (id == 0x0001) {
                size_t f = x + 4;
                if (e.size == 0xFFFFFFFFu && f + 8 <= x + 4 + len) {
                    e.size = u64(&dir[f]);
                    f += 8;
                }
                if (e.compressed == 0xFFFFFFFFu && f + 8 <= x + 4 + len) {
                    e.compressed = u64(&dir[f]);
                    f += 8;
                }
                if (e.offset == 0xFFFFFFFFu && f + 8 <= x + 4 + len)
                    e.offset = u64(&dir[f]);
            }
            x += 4 + len;
        }
        p += 46 + nameLen + extraLen + commentLen;
        entries.push_back(std::move(e));
    }

    if (progress) {
        uint64_t total = 0;
        for (const Entry &e : entries)
            total += e.size;
        progress->total = total;
        progress->done = 0;
    }

    const fs::path root = fs::u8path(folder);
    std::error_code ec;
    fs::create_directories(root, ec);
    std::vector<char> packed, unpacked;
    for (const Entry &e : entries) {
        if (progress && progress->cancel && *progress->cancel) {
            error = "Cancelled";
            return false;
        }
        const fs::path target = safePath(root, e.name);
        if (target.empty()) {
            error = "The zip file has an unsafe path: " + e.name;
            return false;
        }
        if (!e.name.empty() && (e.name.back() == '/' || e.name.back() == '\\')) {
            fs::create_directories(target, ec);
            continue;
        }
        if (e.flags & 1) {
            error = "The zip file is encrypted";
            return false;
        }
        if (e.method != 0 && e.method != 8) {
            error = "The zip file uses a compression this can't read (method " + std::to_string(e.method) + ")";
            return false;
        }
        if (e.size > 0x7FFFFFFFu || e.compressed > 0x7FFFFFFFu) {
            error = e.name + " is too big to unpack (over 2 GB)";
            return false;
        }
        unsigned char local[30];
        if (!readAt(in, e.offset, local, sizeof(local)) || u32(local) != 0x04034b50) {
            error = "The zip file is damaged at " + e.name;
            return false;
        }
        const uint64_t dataAt = e.offset + 30 + u16(local + 26) + u16(local + 28);
        packed.resize(static_cast<size_t>(e.compressed));
        if (!packed.empty() && !readAt(in, dataAt, packed.data(), packed.size())) {
            error = "The zip file is cut short at " + e.name;
            return false;
        }
        const char *data = packed.data();
        if (e.method == 8) {
            unpacked.resize(static_cast<size_t>(e.size));
            const int got = e.size == 0 ? 0
                                        : stbi_zlib_decode_noheader_buffer(unpacked.data(), static_cast<int>(unpacked.size()),
                                                                           packed.data(), static_cast<int>(packed.size()));
            if (got != static_cast<int>(e.size)) {
                error = "The zip file is damaged at " + e.name;
                return false;
            }
            data = unpacked.data();
        } else if (e.compressed != e.size) {
            error = "The zip file is damaged at " + e.name;
            return false;
        }
        if (crc32(data, static_cast<size_t>(e.size)) != e.crc) {
            error = "The zip file is damaged at " + e.name + " (checksum)";
            return false;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(data, static_cast<std::streamsize>(e.size));
        if (!out) {
            error = "Can't write " + target.string() + " (is the disk full?)";
            return false;
        }
        if (progress)
            progress->done += e.size;
    }
    return true;
}

bool extractArchive(const std::string &archive, const std::string &folder, std::string &error, ExtractProgress *progress)
{
    const std::string name = lower(archive);
    if (endsWith(name, ".zip"))
        return extractZip(archive, folder, error, progress);
    if (!isArchive(archive)) {
        error = "Not an archive this can unpack: " + fs::u8path(archive).filename().string();
        return false;
    }
#ifdef _WIN32
    // Windows' own tar is bsdtar, which reads more than tar files
    const char *root = std::getenv("SystemRoot");
    const std::string tar = std::string(root ? root : "C:\\Windows") + "\\System32\\tar.exe";
#else
    const std::string tar = "/usr/bin/tar";
#endif
    std::error_code ec;
    fs::create_directories(fs::u8path(folder), ec);
    int code = 0;
    if (!engine::runProgram({tar, "-xf", archive, "-C", folder}, code, error))
        return false;
    if (code != 0) {
        error = "Couldn't unpack " + fs::u8path(archive).filename().string() +
                (endsWith(name, ".7z") || endsWith(name, ".xz") ? " (the system's tar may not read this kind)" : "");
        return false;
    }
    return true;
}

}
