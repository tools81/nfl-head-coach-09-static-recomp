// Audio driver (XAudio*) exports. XMA contexts are in xboxkrnl_xma.cpp.
//
// Null implementation for now: registered render clients are called back every 256
// samples at 48 kHz (as the hardware does) on a dedicated guest thread, and submitted
// frames are discarded.

#include <bit>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "kernel/guest.h"
#include "kernel/host_audio.h"
#include "kernel/heap.h"
#include "kernel/threads.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kMaxClients = 8;
        constexpr uint32_t kDriverTag = 0x41550000;  // 'AU' | index
        constexpr auto kFrameInterval = std::chrono::nanoseconds(1'000'000'000ll * 256 / 48000);

        struct Client
        {
            uint32_t callback = 0;
            uint32_t argument = 0;
        };

        std::mutex g_audioMutex;
        Client g_clients[kMaxClients];
        bool g_audioThreadStarted = false;

        void AudioThread(GuestThread* thread)
        {
            PPCContext ctx{};
            EnterGuestThread(ctx, thread);
            uint8_t* base = g_base;
            // Pace the title's mixer to the output device: request the next frame when the
            // device's queue drops below the target, so latency stays near the target and the
            // host and device clocks cannot drift apart. Until the device exists (it opens on
            // the first submitted frame), fall back to a 256-sample timer.
            constexpr int kTargetQueuedSamples = 48000 * 30 / 1000;  // 30 ms
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                int queued = QueuedAudioSamples();
                if (queued >= 0)
                {
                    while (queued > kTargetQueuedSamples)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        queued = QueuedAudioSamples();
                    }
                    next = std::chrono::steady_clock::now();
                }
                else
                {
                    next += kFrameInterval;
                    std::this_thread::sleep_until(next);
                }
                for (uint32_t i = 0; i < kMaxClients; i++)
                {
                    Client client;
                    {
                        std::lock_guard lock(g_audioMutex);
                        client = g_clients[i];
                    }
                    if (client.callback == 0)
                        continue;
                    const uint64_t stack = ctx.r1.u64;
                    ctx.r3.u64 = client.argument;
                    PPC_CALL_INDIRECT_FUNC(client.callback);
                    ctx.r1.u64 = stack;
                }
            }
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    // (callback_ptr -> { be32 callback; be32 argument }, out_driver)
    const uint32_t callbackPtr = Arg32(ctx, 0), driverPtr = Arg32(ctx, 1);
    const uint32_t callback = Load<uint32_t>(callbackPtr), argument = Load<uint32_t>(callbackPtr + 4);
    std::lock_guard lock(g_audioMutex);
    for (uint32_t i = 0; i < kMaxClients; i++)
    {
        if (g_clients[i].callback != 0)
            continue;
        g_clients[i] = { callback, argument };
        Store<uint32_t>(driverPtr, kDriverTag | i);
        std::fprintf(stderr, "[audio] client %u: callback 0x%08X arg 0x%08X\n", i, callback, argument);
        if (!g_audioThreadStarted)
        {
            g_audioThreadStarted = true;
            std::thread(AudioThread, CreateGuestThread(0x10000)).detach();
        }
        return Return(ctx, status::kSuccess);
    }
    Return(ctx, status::kUnsuccessful);
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    const uint32_t index = Arg32(ctx, 0) & 0xFFFF;
    std::lock_guard lock(g_audioMutex);
    if (index < kMaxClients)
        g_clients[index] = {};
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    // (driver, samples): 256 samples x 6 planar channels of big-endian float.
    constexpr uint32_t kSamples = 256, kChannels = 6;
    float frame[kSamples * kChannels];
    const uint32_t samples = Arg32(ctx, 1);
    for (uint32_t i = 0; i < kSamples * kChannels; i++)
        frame[i] = std::bit_cast<float>(Load<uint32_t>(samples + i * 4));
    SubmitAudioFrame(frame, kSamples);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    Store<uint32_t>(Arg32(ctx, 0), 0x00010001);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    // (category, out_volume float)
    Store<uint32_t>(Arg32(ctx, 1), std::bit_cast<uint32_t>(1.0f));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    // (driver, out_mask)
    Store<uint32_t>(Arg32(ctx, 1), 0);
    Return(ctx, status::kSuccess);
}
