#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace daw {

// Unpacking downloaded archives. Zip files are read here (stored and deflated entries, Zip64,
// CRC-checked); .tar, .tar.gz, .tgz, .tar.bz2, .tar.xz and .7z go to the system's tar (bsdtar,
// part of Windows 10 and later), which reads the ones it was built for. Entries that would land
// outside the folder ("../", absolute paths) are refused. Blocks: call from a background thread.
struct ExtractProgress {
    std::atomic<uint64_t> done{0};  // bytes written
    std::atomic<uint64_t> total{0}; // bytes to write, once known
    const std::atomic<bool> *cancel = nullptr;
};

bool extractArchive(const std::string &archive, const std::string &folder, std::string &error,
                    ExtractProgress *progress = nullptr);
bool extractZip(const std::string &archive, const std::string &folder, std::string &error,
                ExtractProgress *progress = nullptr);
// Whether extractArchive takes a file with this name (by its extension)
bool isArchive(const std::string &name);

// CRC-32 (the zip one), exposed for tests
uint32_t crc32(const void *data, size_t size, uint32_t crc = 0);

}
