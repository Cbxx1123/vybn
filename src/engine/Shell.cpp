#include "Shell.h"

#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
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

// One argument as CommandLineToArgvW reads it back
std::wstring quoteArgument(const std::wstring &arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

}

void openUrl(const std::string &url)
{
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void showInFolder(const std::string &path)
{
    const std::wstring args = L"/select,\"" + fs::u8path(path).make_preferred().wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

bool runProgram(const std::vector<std::string> &args, int &exitCode, std::string &error)
{
    exitCode = -1;
    if (args.empty()) {
        error = "No program to run";
        return false;
    }
    std::wstring line;
    for (const std::string &a : args)
        line += (line.empty() ? L"" : L" ") + quoteArgument(widen(a));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(widen(args[0]).c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process)) {
        error = "Couldn't run " + args[0] + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    exitCode = static_cast<int>(code);
    return true;
}

#else

void openUrl(const std::string &) {}
void showInFolder(const std::string &) {}

bool runProgram(const std::vector<std::string> &, int &exitCode, std::string &error)
{
    exitCode = -1;
    error = "Running programs isn't supported on this platform yet";
    return false;
}

#endif

}
