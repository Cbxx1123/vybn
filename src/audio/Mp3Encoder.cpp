#include "Mp3Encoder.h"

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstdio>

namespace audio {

namespace {

// Newer than MinGW's headers: the sink writer's container attribute and its MP3 container
const GUID kTranscodeContainerType = {0x150ff23f, 0x4abc, 0x478b, {0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca}};
const GUID kContainerMp3 = {0xe438b912, 0x83f1, 0x4de6, {0x9e, 0x3a, 0x9f, 0xfb, 0xc6, 0xdd, 0x24, 0xd1}};

// Frames handed to the encoder per sample
constexpr uint64_t kPiece = 8192;

// Media Foundation is loaded when an MP3 is first written rather than linked, so the program still
// starts on Windows editions without it (the N editions, until the Media Feature Pack is added)
struct Api {
    HRESULT(WINAPI *startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI *shutdown)() = nullptr;
    HRESULT(WINAPI *createAttributes)(IMFAttributes **, UINT32) = nullptr;
    HRESULT(WINAPI *createMediaType)(IMFMediaType **) = nullptr;
    HRESULT(WINAPI *createMemoryBuffer)(DWORD, IMFMediaBuffer **) = nullptr;
    HRESULT(WINAPI *createSample)(IMFSample **) = nullptr;
    HRESULT(WINAPI *createSinkWriter)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **) = nullptr;
    bool ok = false;
};

template <typename F>
bool load(HMODULE module, const char *name, F &out)
{
    out = module ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(module, name))) : nullptr;
    return out != nullptr;
}

const Api &api()
{
    static const Api loaded = [] {
        Api a;
        HMODULE plat = LoadLibraryW(L"mfplat.dll");
        HMODULE readWrite = LoadLibraryW(L"mfreadwrite.dll");
        a.ok = load(plat, "MFStartup", a.startup) && load(plat, "MFShutdown", a.shutdown) &&
               load(plat, "MFCreateAttributes", a.createAttributes) && load(plat, "MFCreateMediaType", a.createMediaType) &&
               load(plat, "MFCreateMemoryBuffer", a.createMemoryBuffer) && load(plat, "MFCreateSample", a.createSample) &&
               load(readWrite, "MFCreateSinkWriterFromURL", a.createSinkWriter);
        return a;
    }();
    return loaded;
}

template <typename T>
void release(T *&p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::wstring widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string hex(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

}

struct Mp3Encoder::State {
    bool com = false;     // CoInitializeEx succeeded here, so CoUninitialize is owed
    bool started = false; // MFStartup succeeded, so MFShutdown is owed
    IMFSinkWriter *writer = nullptr;
    DWORD stream = 0;
    uint32_t rate = 0;
    uint32_t channels = 0;
    uint64_t frames = 0; // written so far, for the samples' times

    ~State()
    {
        release(writer);
        if (started)
            api().shutdown();
        if (com)
            CoUninitialize();
    }
};

Mp3Encoder::Mp3Encoder() = default;
Mp3Encoder::~Mp3Encoder() = default;

bool Mp3Encoder::open(const std::string &path, uint32_t sampleRate, uint32_t channels, int kbps, std::string &error)
{
    state_ = std::make_unique<State>();
    State &s = *state_;
    const Api &a = api();
    if (!a.ok) {
        error = "MP3 export needs Windows' Media Foundation, which isn't installed (on Windows N, add the Media Feature Pack)";
        return false;
    }
    // Any apartment will do: one already set up on this thread (RPC_E_CHANGED_MODE) is used as it is
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s.com = SUCCEEDED(com);
    HRESULT hr = a.startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        error = "Media Foundation didn't start (" + hex(hr) + ")";
        return false;
    }
    s.started = true;

    IMFAttributes *attributes = nullptr;
    IMFMediaType *output = nullptr;
    IMFMediaType *input = nullptr;
    hr = a.createAttributes(&attributes, 1);
    if (SUCCEEDED(hr))
        hr = attributes->SetGUID(kTranscodeContainerType, kContainerMp3);
    if (SUCCEEDED(hr))
        hr = a.createSinkWriter(widen(path).c_str(), nullptr, attributes, &s.writer);
    if (FAILED(hr)) {
        release(attributes);
        error = "Couldn't create " + path + " (" + hex(hr) + ")";
        return false;
    }

    // What the file holds: MP3 at a constant bitrate
    hr = a.createMediaType(&output);
    if (SUCCEEDED(hr)) {
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
        output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(kbps) * 1000 / 8);
        hr = s.writer->AddStream(output, &s.stream);
    }
    // What it's given: 16-bit PCM
    if (SUCCEEDED(hr))
        hr = a.createMediaType(&input);
    if (SUCCEEDED(hr)) {
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
        input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        input->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        input->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * 2);
        hr = s.writer->SetInputMediaType(s.stream, input, nullptr);
    }
    if (SUCCEEDED(hr))
        hr = s.writer->BeginWriting();
    release(attributes);
    release(output);
    release(input);
    if (FAILED(hr)) {
        error = "Windows' MP3 encoder can't write " + std::to_string(kbps) + " kbps at " + std::to_string(sampleRate) +
                " Hz (" + hex(hr) + ")";
        return false;
    }
    s.rate = sampleRate;
    s.channels = channels;
    return true;
}

bool Mp3Encoder::write(const int16_t *interleaved, uint64_t frames, std::string &error)
{
    if (!state_ || !state_->writer) {
        error = "The MP3 encoder isn't open";
        return false;
    }
    State &s = *state_;
    const Api &a = api();
    for (uint64_t at = 0; at < frames; at += kPiece) {
        const uint64_t n = std::min(kPiece, frames - at);
        const DWORD bytes = static_cast<DWORD>(n * s.channels * sizeof(int16_t));
        IMFMediaBuffer *buffer = nullptr;
        IMFSample *sample = nullptr;
        HRESULT hr = a.createMemoryBuffer(bytes, &buffer);
        BYTE *data = nullptr;
        if (SUCCEEDED(hr))
            hr = buffer->Lock(&data, nullptr, nullptr);
        if (SUCCEEDED(hr)) {
            std::copy_n(reinterpret_cast<const BYTE *>(interleaved + at * s.channels), bytes, data);
            buffer->Unlock();
            hr = buffer->SetCurrentLength(bytes);
        }
        if (SUCCEEDED(hr))
            hr = a.createSample(&sample);
        if (SUCCEEDED(hr))
            hr = sample->AddBuffer(buffer);
        // Times in 100 ns units, from the frame count so they never drift
        const LONGLONG start = static_cast<LONGLONG>(s.frames * 10000000ull / s.rate);
        const LONGLONG end = static_cast<LONGLONG>((s.frames + n) * 10000000ull / s.rate);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleTime(start);
        if (SUCCEEDED(hr))
            hr = sample->SetSampleDuration(end - start);
        if (SUCCEEDED(hr))
            hr = s.writer->WriteSample(s.stream, sample);
        release(sample);
        release(buffer);
        if (FAILED(hr)) {
            error = "The MP3 encoder failed (" + hex(hr) + ")";
            return false;
        }
        s.frames += n;
    }
    return true;
}

bool Mp3Encoder::finish(std::string &error)
{
    if (!state_ || !state_->writer) {
        error = "The MP3 encoder isn't open";
        return false;
    }
    const HRESULT hr = state_->writer->Finalize();
    state_.reset(); // releases the writer, which closes the file
    if (FAILED(hr)) {
        error = "The MP3 encoder couldn't finish the file (" + hex(hr) + ")";
        return false;
    }
    return true;
}

}

#else

namespace audio {

struct Mp3Encoder::State {};

Mp3Encoder::Mp3Encoder() = default;
Mp3Encoder::~Mp3Encoder() = default;

bool Mp3Encoder::open(const std::string &, uint32_t, uint32_t, int, std::string &error)
{
    error = "MP3 export is only available on Windows";
    return false;
}

bool Mp3Encoder::write(const int16_t *, uint64_t, std::string &error)
{
    error = "MP3 export is only available on Windows";
    return false;
}

bool Mp3Encoder::finish(std::string &error)
{
    error = "MP3 export is only available on Windows";
    return false;
}

}

#endif
