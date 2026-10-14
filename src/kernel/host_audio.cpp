#include "kernel/host_audio.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace hc09
{
    namespace
    {
        constexpr uint32_t kSampleRate = 48000;
        constexpr REFERENCE_TIME kBufferDuration = 1000000;  // 100 ms, in 100 ns units

        struct Output
        {
            std::mutex mutex;
            bool initialized = false;
            bool available = false;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioRenderClient> render;
            UINT32 bufferFrames = 0;
            bool started = false;
        };

        Output& GetOutput()
        {
            static Output output;
            return output;
        }

        bool Initialize(Output& out)
        {
            if (std::getenv("HC09_NO_AUDIO"))
                return false;
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
                FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
                FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(out.client.GetAddressOf()))))
            {
                std::fprintf(stderr, "[audio] no output device\n");
                return false;
            }
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            format.nChannels = 2;
            format.nSamplesPerSec = kSampleRate;
            format.wBitsPerSample = 32;
            format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
            format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
            const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            if (FAILED(out.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &format, nullptr)) ||
                FAILED(out.client->GetBufferSize(&out.bufferFrames)) ||
                FAILED(out.client->GetService(IID_PPV_ARGS(&out.render))))
            {
                std::fprintf(stderr, "[audio] output initialization failed\n");
                return false;
            }
            std::fprintf(stderr, "[audio] output: 48 kHz stereo, %u-frame buffer\n", out.bufferFrames);
            return true;
        }
    }

    int QueuedAudioSamples()
    {
        Output& out = GetOutput();
        std::lock_guard lock(out.mutex);
        if (!out.initialized || !out.available)
            return -1;
        UINT32 padding = 0;
        if (FAILED(out.client->GetCurrentPadding(&padding)))
            return -1;
        return int(padding);
    }

    void SubmitAudioFrame(const float* planar, uint32_t samples)
    {
        Output& out = GetOutput();
        std::lock_guard lock(out.mutex);
        if (!out.initialized)
        {
            out.initialized = true;
            out.available = Initialize(out);
        }
        if (!out.available)
            return;

        // HC09_AUDIO_STATS=1: every 5 s, the peak of what the title submitted (all channels)
        // and how many frames had to be dropped because the device buffer was full.
        static float peak = 0.0f;
        static uint32_t counted = 0, frames = 0, dropped = 0;
        static const bool stats = std::getenv("HC09_AUDIO_STATS") != nullptr;
        if (stats)
        {
            for (uint32_t i = 0; i < samples * 6; i++)
                peak = std::max(peak, std::abs(planar[i]));
            counted += samples;
            frames++;
            if (counted >= kSampleRate * 5)
            {
                std::fprintf(stderr, "[audio] submitted peak %.3f, %u frames, %u dropped (device full)\n", peak, frames, dropped);
                peak = 0.0f;
                counted = frames = dropped = 0;
            }
        }

        UINT32 padding = 0;
        const bool paddingOk = SUCCEEDED(out.client->GetCurrentPadding(&padding));
        if (stats)
        {
            static uint64_t paddingSum = 0, paddingCount = 0;
            static UINT32 paddingMax = 0;
            paddingSum += padding;
            paddingCount++;
            paddingMax = std::max(paddingMax, padding);
            if (paddingCount == 938)
            {
                std::fprintf(stderr, "[audio] device queue: average %.1f ms, max %.1f ms\n",
                    double(paddingSum) / paddingCount * 1000.0 / kSampleRate, paddingMax * 1000.0 / kSampleRate);
                paddingSum = paddingCount = 0;
                paddingMax = 0;
            }
        }
        if (!paddingOk || out.bufferFrames - padding < samples)
        {
            dropped++;
            return;  // device buffer full: drop this frame
        }
        BYTE* data = nullptr;
        if (FAILED(out.render->GetBuffer(samples, &data)))
            return;
        // ITU downmix of 5.1 to stereo (LFE omitted).
        constexpr float kSide = 0.7071f;
        const float* fl = planar;
        const float* fr = planar + samples;
        const float* c = planar + samples * 2;
        const float* sl = planar + samples * 4;
        const float* sr = planar + samples * 5;
        auto* dest = reinterpret_cast<float*>(data);
        for (uint32_t i = 0; i < samples; i++)
        {
            dest[i * 2] = std::clamp(fl[i] + kSide * (c[i] + sl[i]), -1.0f, 1.0f);
            dest[i * 2 + 1] = std::clamp(fr[i] + kSide * (c[i] + sr[i]), -1.0f, 1.0f);
        }
        // HC09_AUDIO_RECORD=<path.wav>: also record the stereo output (16-bit, 48 kHz).
        static FILE* record = [] {
            const char* path = std::getenv("HC09_AUDIO_RECORD");
            FILE* file = path ? std::fopen(path, "wb") : nullptr;
            if (file)
            {
                const uint8_t header[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0,
                    1, 0, 2, 0, 0x80, 0xBB, 0, 0, 0x00, 0xEE, 0x02, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0 };
                std::fwrite(header, 1, sizeof(header), file);
            }
            return file;
        }();
        if (record)
        {
            static uint32_t dataBytes = 0;
            int16_t pcm[512 * 2];
            const uint32_t count = std::min<uint32_t>(samples, 512);
            for (uint32_t i = 0; i < count * 2; i++)
                pcm[i] = int16_t(dest[i] * 32767.0f);
            std::fwrite(pcm, 4, count, record);
            dataBytes += count * 4;
            // Keep the header's sizes current so the file is valid whenever the game is closed.
            const long end = std::ftell(record);
            const uint32_t riffSize = dataBytes + 36;
            std::fseek(record, 4, SEEK_SET);
            std::fwrite(&riffSize, 4, 1, record);
            std::fseek(record, 40, SEEK_SET);
            std::fwrite(&dataBytes, 4, 1, record);
            std::fseek(record, end, SEEK_SET);
            std::fflush(record);
        }
        out.render->ReleaseBuffer(samples, 0);
        // Start once a little audio is queued, so the device doesn't begin with an underrun.
        if (!out.started && padding + samples >= kSampleRate / 50)
        {
            out.client->Start();
            out.started = true;
        }
    }
}
