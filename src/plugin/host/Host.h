#pragma once

// The plugin host's parts its plugin formats share (host/HostMain.cpp): the shared memory, the news
// the plugin's threads leave for the main thread, the embedded interface's frame window. And a
// Format: one plugin standard (CLAP, VST3) behind the app's requests.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // Windows 10 APIs; MinGW's headers otherwise stop at XP
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "plugin/Protocol.h"

namespace host {

extern plugin::Shared *shared;

// News for the main thread to send (any thread may set it; wake() makes the main thread look)
extern std::atomic<bool> restartRequested; // deactivate and activate the plugin again
extern std::atomic<bool> latencyChanged;
extern std::atomic<bool> stateDirty;
extern std::atomic<uint32_t> paramsRescan; // CLAP's rescan flags (values 1, text 2, info 4, all 8), top bit set
extern std::atomic<bool> guiClosed;        // the user closed the plugin's own window
void wake();

// Log severities, CLAP's numbers (the app's)
enum LogLevel { kLogDebug = 0, kLogInfo = 1, kLogWarning = 2, kLogError = 3 };
void log(int severity, const std::string &text); // any thread

bool isMainThread();
bool isAudioThread();

// Whether the plugin is active, so its audio thread may process (main thread)
bool pluginActive();

// The embedded interface's frame window grown or shrunk to a size; false when there is none
bool resizeFrame(uint32_t width, uint32_t height);

std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);

// One plugin standard. The host makes one, by the file it is given, and hands it the requests.
class Format {
public:
    virtual ~Format() = default;

    // Main thread. The plugin's file is opened once, by the first request
    virtual bool open(const std::string &path, std::string &error) = 0;
    virtual bool scan(std::vector<plugin::Descriptor> &found, std::string &error) = 0;
    // The channels of the plugin's sidechain input (0 for none): a plugin made, asked, destroyed
    virtual bool probe(const std::string &id, uint32_t &sidechain, std::string &error) = 0;
    virtual bool load(const std::string &id, plugin::LoadedInfo &info, std::string &error) = 0;
    virtual bool loaded() const = 0;
    // Prepares to process at a rate (shared->latency set); deactivate runs once the audio thread
    // has stopped processing
    virtual bool activate(double rate, std::string &error) = 0;
    virtual void deactivate() = 0;
    virtual uint32_t latency() = 0;
    virtual std::vector<plugin::ParamInfo> params() = 0;
    virtual bool paramText(uint32_t id, double value, std::string &text) = 0;
    virtual bool hasState() const = 0;
    virtual bool saveState(std::string &out, std::string &error) = 0;
    virtual bool loadState(const std::string &in, std::string &error) = 0;
    // The interface: parented into the frame window (and the frame sized to it), or a window of its own
    virtual bool guiEmbed(HWND frame, std::string &error) = 0;
    virtual void guiResize(uint32_t width, uint32_t height) = 0;
    virtual bool guiOpen(std::string &error) = 0;
    virtual void guiClose() = 0;
    // Each turn of the main loop: what the plugin's threads asked for
    virtual void housekeeping() = 0;
    // Before the host exits: the plugin destroyed, its file let go
    virtual void unload() = 0;

    // Audio thread: one block from the shared memory, its output written there (silence and an
    // error status when it can't); and processing stopped before a deactivate
    virtual void process(uint32_t frames) = 0;
    virtual void stopProcessing() = 0;
};

std::unique_ptr<Format> makeClapFormat();
std::unique_ptr<Format> makeVst3Format();

// The process statuses written to shared->status (CLAP's numbers, which the app reads)
enum ProcessStatus { kProcessError = 0, kProcessContinue = 1, kProcessSleep = 4 };

// Silence in the shared output (audio thread)
void silence(uint32_t frames);

}
