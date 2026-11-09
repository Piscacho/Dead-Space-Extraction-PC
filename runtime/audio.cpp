// Audio output: the AI DMA blocks the game plays (32 kHz stereo) go to the default
// Windows device through WASAPI (shared mode, Windows converts the rate).
//
// The emulation thread pushes samples as each AI block "plays"; a separate thread
// keeps the device buffer topped up from a ring buffer, or with silence when the game
// has not produced enough.
#include "runtime.h"
#include "audio.h"
#include "config.h"
#include <algorithm>
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace
{
constexpr uint32_t kRate = 32000;
constexpr size_t kRingFrames = kRate / 2;  // half a second

std::mutex g_lock;
std::vector<int16_t> g_ring(kRingFrames * 2);
size_t g_read = 0, g_write = 0, g_count = 0;  // in frames
bool g_ready = false;
int g_volume = 256;  // dse.ini volumen (0-100) as 0-256
bool g_starved = true;          // waiting for the ring to refill before playing again
uint64_t g_underruns = 0;
constexpr size_t kResumeFrames = kRate * 50 / 1000;  // refill to 50 ms after running dry

IAudioClient* g_client = nullptr;
IAudioRenderClient* g_render = nullptr;
UINT32 g_bufferFrames = 0;

DWORD WINAPI AudioThread(LPVOID)
{
    for (;;)
    {
        Sleep(4);
        UINT32 padding = 0;
        if (FAILED(g_client->GetCurrentPadding(&padding)))
            continue;
        // Keep about 60 ms queued in the device.
        UINT32 target = kRate * 60 / 1000;
        if (padding >= target)
            continue;
        UINT32 frames = target - padding;
        if (frames > g_bufferFrames - padding)
            frames = g_bufferFrames - padding;
        BYTE* data = nullptr;
        if (FAILED(g_render->GetBuffer(frames, &data)))
            continue;
        auto* out = reinterpret_cast<int16_t*>(data);
        std::lock_guard lock(g_lock);
        // After running dry, stay silent until there is a cushion again: one clean gap
        // instead of crackling, sample-by-sample silence.
        if (g_starved && g_count >= kResumeFrames)
            g_starved = false;
        if (!g_starved && g_count < frames)
        {
            g_starved = true;
            g_underruns++;
        }
        for (UINT32 i = 0; i < frames; i++)
        {
            if (!g_starved && g_count)
            {
                out[i * 2] = g_ring[g_read * 2];
                out[i * 2 + 1] = g_ring[g_read * 2 + 1];
                g_read = (g_read + 1) % kRingFrames;
                g_count--;
            }
            else
                out[i * 2] = out[i * 2 + 1] = 0;
        }
        g_render->ReleaseBuffer(frames, 0);
    }
}
} // namespace

bool AudioInit()
{
    if (getenv("DSE_NOAUDIO"))
        return false;
    g_volume = std::clamp(ConfigInt("volumen", 100), 0, 100) * 256 / 100;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
            (void**)&enumerator)) ||
        FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_client)))
    {
        LOG("[audio] no output device");
        return false;
    }
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = kRate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = kRate * 4;
    REFERENCE_TIME duration = 2000000;  // 200 ms
    HRESULT hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, duration, 0, &fmt, nullptr);
    if (FAILED(hr) || FAILED(g_client->GetBufferSize(&g_bufferFrames)) ||
        FAILED(g_client->GetService(__uuidof(IAudioRenderClient), (void**)&g_render)) || FAILED(g_client->Start()))
    {
        LOG("[audio] WASAPI setup failed (0x%08lX)", (unsigned long)hr);
        return false;
    }
    CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr);
    g_ready = true;
    LOG("[audio] WASAPI output ready (32 kHz stereo, device buffer %u frames)", g_bufferFrames);
    return true;
}

void AudioPushBigEndianRL(const uint8_t* data, uint32_t frames)
{
    if (!g_ready)
        return;
    // Level report every ~5 s of audio (to check sound without listening).
    static uint64_t total = 0;
    static int peak = 0;
    for (uint32_t i = 0; i < frames * 2; i++)
    {
        int v = std::abs(int(int16_t(data[i * 2] << 8 | data[i * 2 + 1])));
        peak = v > peak ? v : peak;
    }
    total += frames;
    if (total >= kRate * 5)
    {
        LOG("[audio] last 5 s: peak level %d of 32767, %llu underruns so far", peak, (unsigned long long)g_underruns);
        total = 0;
        peak = 0;
    }

    std::lock_guard lock(g_lock);
    for (uint32_t i = 0; i < frames; i++)
    {
        if (g_count == kRingFrames)  // full: drop the oldest
        {
            g_read = (g_read + 1) % kRingFrames;
            g_count--;
        }
        const uint8_t* p = data + i * 4;
        int16_t right = int16_t((int16_t(p[0] << 8 | p[1]) * g_volume) >> 8);
        int16_t left = int16_t((int16_t(p[2] << 8 | p[3]) * g_volume) >> 8);
        g_ring[g_write * 2] = left;
        g_ring[g_write * 2 + 1] = right;
        g_write = (g_write + 1) % kRingFrames;
        g_count++;
    }
}

int64_t AudioQueuedFrames()
{
    if (!g_ready)
        return -1;
    std::lock_guard lock(g_lock);
    return int64_t(g_count);
}
