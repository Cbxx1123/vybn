#include "Http.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace engine {

namespace fs = std::filesystem;

#ifdef _WIN32

namespace {

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string systemError(const char *what)
{
    const DWORD code = GetLastError();
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return std::string(what) + ": the server couldn't be found (are you online?)";
    case ERROR_WINHTTP_TIMEOUT:
        return std::string(what) + ": the server took too long to answer";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return std::string(what) + ": couldn't connect to the server";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return std::string(what) + ": the secure connection failed";
    default:
        return std::string(what) + " failed (error " + std::to_string(code) + ")";
    }
}

// Closes a WinHTTP handle when it goes
struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle) : h(handle) {}
    ~Handle()
    {
        if (h)
            WinHttpCloseHandle(h);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    explicit operator bool() const { return h != nullptr; }
};

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

}

bool httpGet(const HttpRequest &request, const std::function<bool(const char *, size_t)> &sink, std::string &error,
             int *status)
{
    if (status)
        *status = 0;
    const std::wstring url = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {};
    wchar_t path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {};
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = "Not a web address: " + request.url;
        return false;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

    // Windows 8.1 and later find the system's proxy themselves; older ones use the default
    Handle session(WinHttpOpen(L"VYBN/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
        session.h = WinHttpOpen(L"VYBN/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = systemError("Opening the connection");
        return false;
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);
    Handle connection(WinHttpConnect(session.h, host, parts.nPort, 0));
    if (!connection) {
        error = systemError("Connecting");
        return false;
    }
    const std::wstring target = std::wstring(path) + extra;
    Handle req(WinHttpOpenRequest(connection.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        error = systemError("Making the request");
        return false;
    }
    std::wstring headers;
    for (const auto &[name, value] : request.headers)
        headers += widen(name) + L": " + widen(value) + L"\r\n";
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        error = systemError("The request");
        return false;
    }

    DWORD code = 0;
    DWORD size = sizeof(code);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &code, &size, WINHTTP_NO_HEADER_INDEX);
    if (status)
        *status = static_cast<int>(code);
    wchar_t lengthText[32] = {};
    size = sizeof(lengthText);
    if (request.progress) {
        request.progress->done = 0;
        request.progress->total = 0;
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &size,
                                WINHTTP_NO_HEADER_INDEX))
            request.progress->total = std::wcstoull(lengthText, nullptr, 10);
    }
    const bool ok = code >= 200 && code < 300;

    std::vector<char> buffer(64 * 1024);
    std::string failureBody; // the start of an error page, for the message
    for (;;) {
        if (request.progress && request.progress->cancel) {
            error = "Cancelled";
            return false;
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(req.h, &available)) {
            error = systemError("Reading");
            return false;
        }
        if (available == 0)
            break;
        DWORD read = 0;
        if (!WinHttpReadData(req.h, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &read)) {
            error = systemError("Reading");
            return false;
        }
        if (read == 0)
            break;
        if (request.progress)
            request.progress->done += read;
        if (!ok) {
            if (failureBody.size() < 400)
                failureBody.append(buffer.data(), std::min<size_t>(read, 400 - failureBody.size()));
            continue;
        }
        if (!sink(buffer.data(), read)) {
            if (error.empty())
                error = "Stopped";
            return false;
        }
    }
    if (!ok) {
        error = "The server answered " + std::to_string(code);
        if (code == 401 || code == 403)
            error += code == 401 ? " (not allowed: check the API key)" : " (refused, or too many requests: try later)";
        else if (code == 404)
            error += " (not found)";
        return false;
    }
    return true;
}

#else

bool httpGet(const HttpRequest &, const std::function<bool(const char *, size_t)> &, std::string &error, int *status)
{
    if (status)
        *status = 0;
    error = "Downloading isn't supported on this platform yet";
    return false;
}

#endif

bool httpGetText(const HttpRequest &request, std::string &body, std::string &error, int *status)
{
    body.clear();
    return httpGet(
        request,
        [&](const char *data, size_t size) {
            if (body.size() + size > 16u * 1024 * 1024) {
                error = "The answer was too long";
                return false;
            }
            body.append(data, size);
            return true;
        },
        error, status);
}

bool httpDownload(const HttpRequest &request, const std::string &path, std::string &error, int *status)
{
    const fs::path target = fs::u8path(path);
    const fs::path part = fs::u8path(path + ".part");
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Can't write " + part.string();
        return false;
    }
    const bool ok = httpGet(
        request,
        [&](const char *data, size_t size) {
            out.write(data, static_cast<std::streamsize>(size));
            if (!out) {
                error = "Can't write " + part.string() + " (is the disk full?)";
                return false;
            }
            return true;
        },
        error, status);
    out.close();
    if (!ok || !out) {
        if (ok)
            error = "Can't write " + part.string();
        fs::remove(part, ec);
        return false;
    }
    fs::remove(target, ec);
    fs::rename(part, target, ec);
    if (ec) {
        error = "Can't rename " + part.string() + ": " + ec.message();
        return false;
    }
    return true;
}

std::string urlEncode(const std::string &text)
{
    static const char *hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}
