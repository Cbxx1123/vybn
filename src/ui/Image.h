#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace ui {

// Pixels for the UI to show as they change (a plugin's window): 4 bytes a pixel, blue green red
// and an unused byte, rows packed. Whatever draws it owns it and bumps `version` when the pixels
// change; renderers keep its texture by `id` and copy the pixels in only when the version moves.
struct ImageData {
    ImageData() : id(next().fetch_add(1) + 1) {}
    ImageData(const ImageData &) = delete;
    ImageData &operator=(const ImageData &) = delete;

    const uint64_t id;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
    uint64_t version = 0;

private:
    static std::atomic<uint64_t> &next()
    {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }
};

}
