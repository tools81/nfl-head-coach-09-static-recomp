// XMA decoder (XMA*) exports, decoded with FFmpeg's XMA2 decoder.
//
// A context is the 64-byte XMA_CONTEXT_DATA block the hardware reads and updates. For every
// enabled context the decoder thread feeds 2048-byte input packets to libavcodec and writes
// the PCM (16-bit big-endian, interleaved when stereo) into the output ring buffer in 256-byte
// blocks, updating the read/write offsets and valid flags as the hardware does. Like the
// hardware it decodes ahead into whatever output space the title has made available.
//
// XMA_CONTEXT_DATA (big-endian dwords, fields listed from bit 0):
//   0: input_buffer_0_packet_count:12 loop_count:8 input_buffer_0_valid:1
//      input_buffer_1_valid:1 output_buffer_block_count:5 output_buffer_write_offset:5
//   1: input_buffer_1_packet_count:12 loop_subframe_start:2 loop_subframe_end:3
//      loop_subframe_skip:3 subframe_decode_count:4 output_buffer_padding:3 sample_rate:2
//      is_stereo:1 unknown:1 output_buffer_valid:1
//   2: input_buffer_read_offset:26 (bits, from the start of the current buffer)
//   3: loop_start:26   4: loop_end:26 packet_metadata:5 current_buffer:1
//   5: input_buffer_0_ptr   6: input_buffer_1_ptr   7: output_buffer_ptr   8: work_buffer_ptr
//   9: output_buffer_read_offset:5

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
}

#include "debug/stack_dump.h"
#include "kernel/guest.h"
#include "kernel/heap.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kContextSize = 0x40;
        constexpr uint32_t kPacketBytes = 2048;
        constexpr uint32_t kPacketHeaderBits = 32;
        constexpr uint32_t kBlockBytes = 256;
        constexpr auto kTick = std::chrono::milliseconds(2);

        struct Field
        {
            uint32_t dword, shift, bits;
        };
        constexpr Field kInput0PacketCount{ 0, 0, 12 };
        constexpr Field kLoopCount{ 0, 12, 8 };
        constexpr Field kInput0Valid{ 0, 20, 1 };
        constexpr Field kInput1Valid{ 0, 21, 1 };
        constexpr Field kOutputBlockCount{ 0, 22, 5 };
        constexpr Field kOutputWriteOffset{ 0, 27, 5 };
        constexpr Field kInput1PacketCount{ 1, 0, 12 };
        constexpr Field kLoopSubframeEnd{ 1, 14, 3 };
        constexpr Field kLoopSubframeSkip{ 1, 17, 3 };
        constexpr Field kSubframeDecodeCount{ 1, 20, 4 };
        constexpr Field kSampleRate{ 1, 27, 2 };
        constexpr Field kIsStereo{ 1, 29, 1 };
        constexpr Field kOutputValid{ 1, 31, 1 };
        constexpr Field kInputReadOffset{ 2, 0, 26 };
        constexpr Field kLoopStart{ 3, 0, 26 };
        constexpr Field kLoopEnd{ 4, 0, 26 };
        constexpr Field kCurrentBuffer{ 4, 31, 1 };
        constexpr Field kInput0Ptr{ 5, 0, 32 };
        constexpr Field kInput1Ptr{ 6, 0, 32 };
        constexpr Field kOutputPtr{ 7, 0, 32 };
        constexpr Field kWorkPtr{ 8, 0, 32 };
        constexpr Field kOutputReadOffset{ 9, 0, 5 };

        uint32_t Get(uint32_t context, Field field)
        {
            const uint32_t value = Load<uint32_t>(context + field.dword * 4) >> field.shift;
            return field.bits == 32 ? value : value & ((1u << field.bits) - 1);
        }

        void Set(uint32_t context, Field field, uint32_t value)
        {
            const uint32_t address = context + field.dword * 4;
            const uint32_t mask = field.bits == 32 ? ~0u : ((1u << field.bits) - 1) << field.shift;
            Store<uint32_t>(address, (Load<uint32_t>(address) & ~mask) | ((value << field.shift) & mask));
        }

        uint32_t SampleRate(uint32_t context)
        {
            static constexpr uint32_t kRates[4] = { 24000, 32000, 44100, 48000 };
            return kRates[Get(context, kSampleRate)];
        }

        // Diagnostics, printed every 5 s by the decoder thread.
        struct Counters
        {
            uint64_t packets = 0, sendErrors = 0, samples = 0, blocks = 0, steps = 0, noOutputSpace = 0, noInput = 0;
            uint64_t frames = 0, repeatedFrames = 0;
            uint64_t lateTicks = 0, maxTickGapMs = 0;   // decoder wake-ups over 10 ms late; worst gap
            uint64_t drained = 0;  // the title emptied an output ring while input was still pending  // decoded frames; non-silent ones repeating a recent frame
            float peak = 0.0f;  // largest decoded sample magnitude
        } g_counters;

        // Host-side decoder for one context: an FFmpeg XMA2 decoder and the PCM it produced
        // that has not been written to the output buffer yet.
        struct ContextState
        {
            AVCodecContext* codec = nullptr;
            AVPacket* packet = nullptr;
            AVFrame* frame = nullptr;
            uint32_t channels = 0, sampleRate = 0;
            std::deque<int16_t> pcm;  // interleaved
            std::deque<uint64_t> recentHashes;  // diagnostics: hashes of recent decoded frames
            // XMADisableContext only pauses a context: like the hardware's, its decode position and
            // decoder state survive until it is re-enabled. Only initialization resets them.
            bool enabled = true;
            bool failed = false;

            ContextState() = default;
            ContextState(const ContextState&) = delete;
            ContextState& operator=(const ContextState&) = delete;
            ~ContextState() { Close(); }

            void Close()
            {
                avcodec_free_context(&codec);
                av_packet_free(&packet);
                av_frame_free(&frame);
                pcm.clear();
            }

            // (Re)creates the decoder for the context's channel count and rate.
            bool Open(uint32_t newChannels, uint32_t newRate)
            {
                if (codec && channels == newChannels && sampleRate == newRate)
                    return true;
                Close();
                channels = newChannels;
                sampleRate = newRate;
                static const bool quiet = [] { av_log_set_level(AV_LOG_ERROR); return true; }();
                (void)quiet;
                const AVCodec* xma = avcodec_find_decoder(AV_CODEC_ID_XMA2);
                codec = xma ? avcodec_alloc_context3(xma) : nullptr;
                if (!codec)
                    return false;
                codec->sample_rate = int(sampleRate);
                codec->block_align = int(kPacketBytes);
                av_channel_layout_default(&codec->ch_layout, int(channels));
                // XMA2WAVEFORMATEX (34 bytes): u16 stream count, u32 channel mask, ...
                codec->extradata = static_cast<uint8_t*>(av_mallocz(34 + AV_INPUT_BUFFER_PADDING_SIZE));
                codec->extradata_size = 34;
                codec->extradata[0] = 1;  // one stream
                const uint32_t mask = channels == 2 ? 0x3 : 0x4;  // FL|FR, or FC
                std::memcpy(codec->extradata + 2, &mask, 4);
                packet = av_packet_alloc();
                frame = av_frame_alloc();
                if (avcodec_open2(codec, xma, nullptr) < 0 || !packet || !frame)
                {
                    std::fprintf(stderr, "[xma] decoder initialization failed\n");
                    Close();
                    return false;
                }
                return true;
            }

            // Decodes one 2048-byte packet, appending its PCM.
            void Decode(const uint8_t* data)
            {
                if (av_new_packet(packet, int(kPacketBytes)) < 0)
                    return;
                std::memcpy(packet->data, data, kPacketBytes);
                const int sent = avcodec_send_packet(codec, packet);
                av_packet_unref(packet);
                g_counters.packets++;
                if (sent < 0)
                {
                    g_counters.sendErrors++;
                    static int reports = 0;
                    if (reports++ < 5)
                    {
                        char text[128];
                        av_strerror(sent, text, sizeof(text));
                        std::fprintf(stderr, "[xma] packet rejected: %s (header %02X%02X%02X%02X, %u ch, %u Hz)%c", text, data[0], data[1],
                            data[2], data[3], channels, sampleRate, 10);
                    }
                    // Keep the stream moving: the packet's frames (512 samples each) as silence,
                    // so the title keeps supplying packets and the decoder can resynchronize.
                    const uint32_t frames = std::max<uint32_t>(data[0] >> 2, 1);
                    pcm.insert(pcm.end(), size_t(frames) * 512 * channels, int16_t(0));
                    return;
                }
                while (avcodec_receive_frame(codec, frame) == 0)
                {
                    const int frameChannels = frame->ch_layout.nb_channels;
                    // Diagnostics: decoded frames identical to a recent frame of this stream
                    // (silence excluded).
                    {
                        uint64_t hash = 0xCBF29CE484222325ull;
                        bool silent = true;
                        const auto* samples0 = reinterpret_cast<const uint32_t*>(frame->extended_data[0]);
                        for (int i = 0; i < frame->nb_samples; i++)
                        {
                            hash = (hash ^ samples0[i]) * 0x100000001B3ull;
                            silent = silent && (samples0[i] & 0x7FFFFFFF) == 0;
                        }
                        g_counters.frames++;
                        if (!silent)
                        {
                            if (std::find(recentHashes.begin(), recentHashes.end(), hash) != recentHashes.end())
                                g_counters.repeatedFrames++;
                            recentHashes.push_back(hash);
                            if (recentHashes.size() > 64)
                                recentHashes.pop_front();
                        }
                    }
                    for (int i = 0; i < frame->nb_samples; i++)
                    {
                        for (uint32_t c = 0; c < channels; c++)
                        {
                            const int source = std::min<int>(int(c), frameChannels - 1);
                            const float value = reinterpret_cast<const float*>(frame->extended_data[source])[i];
                            g_counters.peak = std::max(g_counters.peak, std::abs(value));
                            pcm.push_back(int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
                        }
                    }
                    g_counters.samples += uint64_t(frame->nb_samples);
                    av_frame_unref(frame);
                }
            }
        };

        std::mutex g_xmaMutex;
        std::unordered_map<uint32_t, std::unique_ptr<ContextState>> g_enabled;  // context address -> decoder
        bool g_threadStarted = false;

        // Moves the read position to the next packet; switches buffers (invalidating the
        // finished one) at the end of a buffer. Returns false when no input remains.
        bool AdvancePacket(uint32_t context)
        {
            const bool second = Get(context, kCurrentBuffer) != 0;
            const uint32_t packetCount = Get(context, second ? kInput1PacketCount : kInput0PacketCount);
            const uint32_t nextPacket = Get(context, kInputReadOffset) / (kPacketBytes * 8) + 1;
            if (nextPacket < packetCount)
            {
                Set(context, kInputReadOffset, nextPacket * kPacketBytes * 8 + kPacketHeaderBits);
                return true;
            }
            Set(context, second ? kInput1Valid : kInput0Valid, 0);
            Set(context, kCurrentBuffer, second ? 0 : 1);
            Set(context, kInputReadOffset, kPacketHeaderBits);
            return Get(context, second ? kInput0Valid : kInput1Valid) != 0;
        }

        bool HasInput(uint32_t context)
        {
            const bool second = Get(context, kCurrentBuffer) != 0;
            if (Get(context, second ? kInput1Valid : kInput0Valid))
                return true;
            // The current buffer is empty but the other one was supplied: switch to it.
            if (Get(context, second ? kInput0Valid : kInput1Valid))
            {
                Set(context, kCurrentBuffer, second ? 0 : 1);
                Set(context, kInputReadOffset, kPacketHeaderBits);
                return true;
            }
            return false;
        }

        // Decodes the packet at the read position and advances past it. False when there is
        // no input left.
        bool DecodeNextPacket(uint32_t context, ContextState& state)
        {
            if (!HasInput(context))
                return false;
            const bool second = Get(context, kCurrentBuffer) != 0;
            const uint32_t buffer = Get(context, second ? kInput1Ptr : kInput0Ptr);
            const uint32_t packet = Get(context, kInputReadOffset) / (kPacketBytes * 8);
            // HC09_XMA_DUMP=1: save the first context's packets (up to 400) to build/xma_dump.bin.
            static const bool dumpPackets = std::getenv("HC09_XMA_DUMP") != nullptr;
            static uint32_t dumpContext = 0, dumped = 0;
            if (dumpPackets && dumped < 400 && (dumpContext == 0 || dumpContext == context))
            {
                dumpContext = context;
                if (FILE* file = std::fopen("build/xma_dump.bin", dumped == 0 ? "wb" : "ab"))
                {
                    std::fwrite(GuestToHost(buffer + packet * kPacketBytes), 1, kPacketBytes, file);
                    std::fclose(file);
                    dumped++;
                }
            }
            state.Decode(GuestToHost(buffer + packet * kPacketBytes));
            AdvancePacket(context);
            return true;
        }

        // One decoder step for a context: fill the output ring buffer's free blocks.
        void Step(uint32_t context, ContextState& state)
        {
            g_counters.steps++;
            if (!Get(context, kOutputValid))
            {
                g_counters.noOutputSpace++;
                return;
            }
            const uint32_t blockCount = Get(context, kOutputBlockCount);
            if (blockCount == 0)
                return;
            const uint32_t channels = Get(context, kIsStereo) ? 2 : 1;
            if (!state.Open(channels, SampleRate(context)))
            {
                if (!state.failed)
                    std::fprintf(stderr, "[xma] context 0x%08X: no decoder\n", context);
                state.failed = true;
                return;
            }
            const uint32_t samplesPerBlock = kBlockBytes / 2;  // int16 values per block (all channels)
            const uint32_t output = Get(context, kOutputPtr);
            const uint32_t readOffset = Get(context, kOutputReadOffset);
            uint32_t writeOffset = Get(context, kOutputWriteOffset);
            // Ring buffer: equal offsets mean empty. Keep one block free so that a non-empty ring
            // never has equal offsets (the title would read that as empty and never consume it).
            uint32_t space = (readOffset > writeOffset ? readOffset - writeOffset : blockCount - writeOffset + readOffset) - 1;
            while (space > 0)
            {
                while (state.pcm.size() < samplesPerBlock && DecodeNextPacket(context, state))
                {
                }
                if (state.pcm.size() < samplesPerBlock)
                {
                    g_counters.noInput++;
                    break;  // out of input: wait for the title to supply more
                }
                g_counters.blocks++;
                auto* block = reinterpret_cast<uint16_t*>(GuestToHost(output + writeOffset * kBlockBytes));
                for (uint32_t i = 0; i < samplesPerBlock; i++)
                {
                    block[i] = __builtin_bswap16(uint16_t(state.pcm.front()));
                    state.pcm.pop_front();
                }
                writeOffset = (writeOffset + 1) % blockCount;
                space--;
            }
            Set(context, kOutputWriteOffset, writeOffset);
            if (space == 0)
                Set(context, kOutputValid, 0);  // full until the title consumes some and re-validates
        }

        void DecoderThread()
        {
            debug::RegisterThread(0xA0);  // shows up as 0xA0 in HC09_THREAD_STATS
            auto lastReport = std::chrono::steady_clock::now();
            auto lastTick = std::chrono::steady_clock::now();
            for (;;)
            {
                std::this_thread::sleep_for(kTick);
                std::lock_guard lock(g_xmaMutex);
                {
                    const auto now = std::chrono::steady_clock::now();
                    const uint64_t gapMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count());
                    lastTick = now;
                    if (gapMs > 10)
                        g_counters.lateTicks++;
                    g_counters.maxTickGapMs = std::max(g_counters.maxTickGapMs, gapMs);
                }
                static const bool stats = std::getenv("HC09_AUDIO_STATS") != nullptr;
                if (stats && std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(5))
                {
                    lastReport = std::chrono::steady_clock::now();
                    std::fprintf(stderr, "[xma] %zu contexts: %llu packets (%llu rejected), %llu samples, %llu blocks; steps %llu, output full %llu, starved %llu, decoded peak %.3f; frames %llu, repeated %llu; late ticks %llu (worst %llu ms), drained %llu%c",
                        g_enabled.size(), (unsigned long long)g_counters.packets, (unsigned long long)g_counters.sendErrors,
                        (unsigned long long)g_counters.samples, (unsigned long long)g_counters.blocks, (unsigned long long)g_counters.steps,
                        (unsigned long long)g_counters.noOutputSpace, (unsigned long long)g_counters.noInput, g_counters.peak, (unsigned long long)g_counters.frames,
                        (unsigned long long)g_counters.repeatedFrames, (unsigned long long)g_counters.lateTicks,
                        (unsigned long long)g_counters.maxTickGapMs, (unsigned long long)g_counters.drained, 10);
                    g_counters = {};
                }
                for (auto& [context, state] : g_enabled)
                {
                    if (state->enabled)
                        Step(context, *state);
                }
            }
        }

        // Runs `body` on the context under the decoder lock.
        template <typename Body>
        auto WithContext(Body body)
        {
            std::lock_guard lock(g_xmaMutex);
            return body();
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__XMACreateContext)
{
    // (out_context): XMA_CONTEXT_DATA lives in physical memory
    const uint32_t context = PhysicalHeap().Allocate(kContextSize, kContextSize);
    if (context)
        std::memset(GuestToHost(context), 0, kContextSize);
    Store<uint32_t>(Arg32(ctx, 0), context);
    Return(ctx, context ? status::kSuccess : status::kNoMemory);
}

PPC_FUNC(__imp__XMAReleaseContext)
{
    const uint32_t context = Arg32(ctx, 0);
    WithContext([&] { g_enabled.erase(context); });
    PhysicalHeap().Free(context);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMAInitializeContext)
{
    // (context, XMA_CONTEXT_INIT: be32 input_buffer_0_ptr, input_buffer_0_packet_count,
    //  input_buffer_1_ptr, input_buffer_1_packet_count, input_buffer_read_offset,
    //  output_buffer_ptr, output_buffer_block_count, work_buffer, subframe_decode_count,
    //  channel_count, sample_rate; XMA_LOOP_DATA: be32 loop_start, loop_end;
    //  u8 loop_count, loop_subframe_end, loop_subframe_skip)
    const uint32_t context = Arg32(ctx, 0), init = Arg32(ctx, 1);
    {
        // Each distinct setup (packet counts, output blocks, subframes, channels, rate, loop data)
        // is logged once.
        static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> seen;
        const auto key = std::make_tuple(Load<uint32_t>(init + 0x04), Load<uint32_t>(init + 0x0C), Load<uint32_t>(init + 0x18),
            Load<uint32_t>(init + 0x20), Load<uint32_t>(init + 0x24), Load<uint32_t>(init + 0x28), Load<uint32_t>(init + 0x2C),
            Load<uint32_t>(init + 0x30), Load<uint32_t>(init + 0x34));
        if (seen.insert(key).second)
            std::fprintf(stderr, "[xma] init: packets %u/%u read offset %u out blocks %u subframes %u channels %u rate %u "
                "loop start %u end %u count/subframe end/skip %08X\n",
                Load<uint32_t>(init + 0x04), Load<uint32_t>(init + 0x0C), Load<uint32_t>(init + 0x10), Load<uint32_t>(init + 0x18),
                Load<uint32_t>(init + 0x20), Load<uint32_t>(init + 0x24), Load<uint32_t>(init + 0x28), Load<uint32_t>(init + 0x2C),
                Load<uint32_t>(init + 0x30), Load<uint32_t>(init + 0x34));
    }
    WithContext([&] {
        g_enabled.erase(context);
        std::memset(GuestToHost(context), 0, kContextSize);
        Set(context, kInput0Ptr, Load<uint32_t>(init + 0x00));
        Set(context, kInput0PacketCount, Load<uint32_t>(init + 0x04));
        Set(context, kInput1Ptr, Load<uint32_t>(init + 0x08));
        Set(context, kInput1PacketCount, Load<uint32_t>(init + 0x0C));
        Set(context, kInputReadOffset, Load<uint32_t>(init + 0x10));
        Set(context, kOutputPtr, Load<uint32_t>(init + 0x14));
        Set(context, kOutputBlockCount, Load<uint32_t>(init + 0x18));
        Set(context, kWorkPtr, Load<uint32_t>(init + 0x1C));
        Set(context, kSubframeDecodeCount, Load<uint32_t>(init + 0x20));
        // Channel field: 0 = mono, 1 = stereo (verified by decoding: 1 only decodes as stereo).
        Set(context, kIsStereo, Load<uint32_t>(init + 0x24) != 0 ? 1 : 0);
        Set(context, kSampleRate, Load<uint32_t>(init + 0x28));
        Set(context, kLoopStart, Load<uint32_t>(init + 0x2C));
        Set(context, kLoopEnd, Load<uint32_t>(init + 0x30));
        Set(context, kLoopCount, Load<uint8_t>(init + 0x34));
        Set(context, kLoopSubframeEnd, Load<uint8_t>(init + 0x35));
        Set(context, kLoopSubframeSkip, Load<uint8_t>(init + 0x36));
    });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMAEnableContext)
{
    const uint32_t context = Arg32(ctx, 0);
    WithContext([&] {
        auto& state = g_enabled[context];
        if (!state)
            state = std::make_unique<ContextState>();
        state->enabled = true;
        if (!g_threadStarted)
        {
            g_threadStarted = true;
            std::thread(DecoderThread).detach();
        }
    });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMADisableContext)
{
    // (context, wait): the decoder only touches contexts under the lock, so it is idle now. The
    // context keeps its decoder state (and any decoded PCM not yet written) for re-enabling.
    const uint32_t context = Arg32(ctx, 0);
    WithContext([&] {
        if (auto found = g_enabled.find(context); found != g_enabled.end())
            found->second->enabled = false;
    });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMASetInputBuffer0)
{
    // (context, buffer, packet_count)
    const uint32_t context = Arg32(ctx, 0);
    WithContext([&] {
        Set(context, kInput0Ptr, Arg32(ctx, 1));
        Set(context, kInput0PacketCount, Arg32(ctx, 2));
    });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMASetInputBuffer1)
{
    const uint32_t context = Arg32(ctx, 0);
    WithContext([&] {
        Set(context, kInput1Ptr, Arg32(ctx, 1));
        Set(context, kInput1PacketCount, Arg32(ctx, 2));
    });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMASetInputBuffer0Valid)
{
    WithContext([&] { Set(Arg32(ctx, 0), kInput0Valid, 1); });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMASetInputBuffer1Valid)
{
    WithContext([&] { Set(Arg32(ctx, 0), kInput1Valid, 1); });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMAIsInputBuffer0Valid)
{
    Return(ctx, WithContext([&] { return Get(Arg32(ctx, 0), kInput0Valid); }));
}

PPC_FUNC(__imp__XMAIsInputBuffer1Valid)
{
    Return(ctx, WithContext([&] { return Get(Arg32(ctx, 0), kInput1Valid); }));
}

PPC_FUNC(__imp__XMASetOutputBufferValid)
{
    WithContext([&] { Set(Arg32(ctx, 0), kOutputValid, 1); });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMAIsOutputBufferValid)
{
    Return(ctx, WithContext([&] { return Get(Arg32(ctx, 0), kOutputValid); }));
}

PPC_FUNC(__imp__XMASetOutputBufferReadOffset)
{
    WithContext([&] {
        const uint32_t context = Arg32(ctx, 0);
        Set(context, kOutputReadOffset, Arg32(ctx, 1));
        if (Arg32(ctx, 1) == Get(context, kOutputWriteOffset) && g_enabled.contains(context) &&
            (Get(context, kInput0Valid) || Get(context, kInput1Valid)))
            g_counters.drained++;
    });
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XMAGetOutputBufferReadOffset)
{
    Return(ctx, WithContext([&] { return Get(Arg32(ctx, 0), kOutputReadOffset); }));
}

PPC_FUNC(__imp__XMAGetOutputBufferWriteOffset)
{
    Return(ctx, WithContext([&] { return Get(Arg32(ctx, 0), kOutputWriteOffset); }));
}
