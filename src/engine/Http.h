#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace engine {

// How far a transfer has got, and a way to stop it: shared with the thread doing it
struct HttpProgress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0}; // 0 while unknown
    std::atomic<bool> cancel{false};
};

struct HttpRequest {
    std::string url;                                          // http:// or https://
    std::vector<std::pair<std::string, std::string>> headers; // sent as they are
    HttpProgress *progress = nullptr;
};

// HTTP GET (WinHTTP on Windows; elsewhere every request fails). Blocks, so call it from a
// background thread. Redirects are followed. The body goes to `sink` as it arrives; returning
// false from it stops the transfer. Returns true on a 2xx response read to the end. On failure
// returns false with the reason in `error`; `status` gets the HTTP status (0 if none came).
bool httpGet(const HttpRequest &request, const std::function<bool(const char *data, size_t size)> &sink,
             std::string &error, int *status = nullptr);
// The whole body as text (for listings: at most 16 MB)
bool httpGetText(const HttpRequest &request, std::string &body, std::string &error, int *status = nullptr);
// The body into a file, written to `path` + ".part" and renamed when complete
bool httpDownload(const HttpRequest &request, const std::string &path, std::string &error, int *status = nullptr);

// Text for a URL's query string: letters, digits and -_.~ as they are, the rest %XX
std::string urlEncode(const std::string &text);

}
