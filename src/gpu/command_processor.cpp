#include "gpu/command_processor.h"

#include <algorithm>
#include <vector>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <tuple>
#include <map>
#include <mutex>
#include <thread>

#include "debug/stack_dump.h"
#include "gpu/draw.h"
#include "gpu/shaders.h"
#include "kernel/guest.h"
#include "kernel/threads.h"

namespace hc09::gpu
{
    namespace
    {
        // PM4 type-3 opcodes
        namespace pm4
        {
            constexpr uint32_t kNop = 0x10;
            constexpr uint32_t kRegRmw = 0x21;
            constexpr uint32_t kDrawIndx = 0x22;
            constexpr uint32_t kWaitForIdle = 0x26;
            constexpr uint32_t kImLoad = 0x27;
            constexpr uint32_t kImLoadImmediate = 0x2B;
            constexpr uint32_t kSetConstant = 0x2D;
            constexpr uint32_t kLoadAluConstant = 0x2F;
            constexpr uint32_t kDrawIndx2 = 0x36;
            constexpr uint32_t kSetConstant2 = 0x55;
            constexpr uint32_t kIndirectBufferPfd = 0x37;
            constexpr uint32_t kWaitRegMem = 0x3C;
            constexpr uint32_t kMemWrite = 0x3D;
            constexpr uint32_t kIndirectBuffer = 0x3F;
            constexpr uint32_t kCondWrite = 0x45;
            constexpr uint32_t kEventWrite = 0x46;
            constexpr uint32_t kMeInit = 0x48;
            constexpr uint32_t kInterrupt = 0x54;
            constexpr uint32_t kEventWriteShd = 0x58;
            constexpr uint32_t kEventWriteExt = 0x59;
            constexpr uint32_t kEventWriteZpd = 0x5B;
            constexpr uint32_t kXeSwap = 0x64;
            constexpr uint32_t kSetBinMask = 0x50;
            constexpr uint32_t kSetBinSelect = 0x51;
            constexpr uint32_t kSetBinMaskLo = 0x60;
            constexpr uint32_t kSetBinMaskHi = 0x61;
            constexpr uint32_t kSetBinSelectLo = 0x62;
            constexpr uint32_t kSetBinSelectHi = 0x63;
        }

        // Predicated tiling: a type-3 packet with header bit 0 set only executes when
        // (bin select & bin mask) != 0. D3D replays a frame's commands once per screen tile
        // and predicates the packets (interrupts, fences, ...) that must run only once.
        uint64_t g_binMask = ~0ull;
        uint64_t g_binSelect = ~0ull;

        constexpr auto kVblankInterval = std::chrono::microseconds(16667);

        uint32_t g_ringBase = 0;        // guest address
        uint32_t g_ringSizeDwords = 0;
        uint32_t g_readPointer = 0;     // dwords
        bool g_tracingFences = false;   // HC09_TRACE_FENCES triggered
        uint32_t g_rptrWriteBack = 0;   // guest address, 0 = disabled
        std::atomic<uint32_t> g_swapCount{ 0 };

        // ---- Interrupts ------------------------------------------------------------------
        //
        // Callbacks run guest code, so they are delivered on a dedicated guest thread.

        // The handler identifies the interrupted hardware thread through the PCR's current
        // CPU byte (D3D acknowledges CP interrupts by clearing that CPU's bit in a mask the
        // GPU waits on), so each interrupt carries its target CPU.
        constexpr uint32_t kPcrCurrentCpu = 0x10C;
        constexpr uint32_t kVblankCpu = 2;

        struct Interrupt
        {
            uint32_t source;  // 0 = vblank, 1 = command processor
            uint32_t cpu;
        };

        std::mutex g_interruptMutex;
        std::condition_variable g_interruptCondition;
        std::deque<Interrupt> g_pendingInterrupts;
        uint32_t g_interruptCallback = 0;
        uint32_t g_interruptUserData = 0;
        uint64_t g_interruptsQueued = 0;     // sequence numbers, under g_interruptMutex
        uint64_t g_interruptsDelivered = 0;
        std::condition_variable g_interruptDelivered;

        // Returns the interrupt's sequence number (0 if there is no handler).
        uint64_t QueueInterrupt(uint32_t source, uint32_t cpu)
        {
            std::lock_guard lock(g_interruptMutex);
            if (g_interruptCallback == 0)
                return 0;
            g_pendingInterrupts.push_back({ source, cpu });
            g_interruptCondition.notify_one();
            return ++g_interruptsQueued;
        }

        // Blocks until the handler has run for every interrupt up to `sequence`. D3D's CP
        // interrupt handler reads a mailbox the command stream wrote just before the INTERRUPT
        // packet, so the command processor must not run ahead (and overwrite it) meanwhile.
        void WaitForInterrupt(uint64_t sequence)
        {
            std::unique_lock lock(g_interruptMutex);
            g_interruptDelivered.wait(lock, [&] { return g_interruptsDelivered >= sequence; });
        }

        void InterruptThread(GuestThread* thread)
        {
            PPCContext ctx{};
            EnterGuestThread(ctx, thread);
            uint8_t* base = g_base;
            for (;;)
            {
                Interrupt interrupt;
                uint32_t callback, userData;
                {
                    std::unique_lock lock(g_interruptMutex);
                    g_interruptCondition.wait(lock, [] { return !g_pendingInterrupts.empty(); });
                    interrupt = g_pendingInterrupts.front();
                    g_pendingInterrupts.pop_front();
                    callback = g_interruptCallback;
                    userData = g_interruptUserData;
                }
                Store<uint8_t>(thread->pcr + kPcrCurrentCpu, uint8_t(interrupt.cpu));
                const uint32_t source = interrupt.source;
                const uint64_t stack = ctx.r1.u64;
                ctx.r3.u64 = source;
                ctx.r4.u64 = userData;
                PPC_CALL_INDIRECT_FUNC(callback);
                ctx.r1.u64 = stack;
                {
                    std::lock_guard lock(g_interruptMutex);
                    g_interruptsDelivered++;
                }
                g_interruptDelivered.notify_all();
            }
        }

        void VblankThread()
        {
            auto next = std::chrono::steady_clock::now();
            for (;;)
            {
                next += kVblankInterval;
                std::this_thread::sleep_until(next);
                WriteRegister(reg::kD1ModeVblankStatus, ReadRegister(reg::kD1ModeVblankStatus) | 1);
                QueueInterrupt(0, kVblankCpu);
            }
        }

        // ---- GPU memory access -----------------------------------------------------------
        //
        // The GPU sees memory as little-endian dwords; the low two bits of a PM4 address
        // select a byte-swap mode that converts to the logical value.

        uint32_t GpuSwap(uint32_t value, uint32_t mode)
        {
            switch (mode & 3)
            {
            case 1: return ((value & 0x00FF00FF) << 8) | ((value >> 8) & 0x00FF00FF);  // 8-in-16
            case 2: return __builtin_bswap32(value);                                    // 8-in-32
            case 3: return (value << 16) | (value >> 16);                               // 16-in-32
            default: return value;
            }
        }

        uint32_t GpuRead(uint32_t address)
        {
            uint32_t raw;
            std::memcpy(&raw, GuestToHost(PhysicalToGuest(address & ~3u)), 4);
            return GpuSwap(raw, address);
        }

        void GpuWrite(uint32_t address, uint32_t value)
        {
            const uint32_t raw = GpuSwap(value, address);
            std::memcpy(GuestToHost(PhysicalToGuest(address & ~3u)), &raw, 4);
        }

        bool CompareFunction(uint32_t function, uint32_t value, uint32_t reference)
        {
            switch (function & 7)
            {
            case 0: return false;  // never
            case 1: return value < reference;
            case 2: return value <= reference;
            case 3: return value == reference;
            case 4: return value != reference;
            case 5: return value >= reference;
            case 6: return value > reference;
            default: return true;
            }
        }

        // ---- Packet execution ------------------------------------------------------------

        class Reader
        {
        public:
            Reader(uint32_t base, uint32_t sizeDwords, uint32_t index) : base_(base), size_(sizeDwords), index_(index) {}
            uint32_t Read()
            {
                const uint32_t value = Load<uint32_t>(base_ + index_ * 4);
                index_ = (index_ + 1) % size_;
                return value;
            }
            uint32_t Peek() const { return Load<uint32_t>(base_ + index_ * 4); }
            void Skip(uint32_t count) { index_ = (index_ + count) % size_; }
            uint32_t index() const { return index_; }
            uint32_t size() const { return size_; }

        private:
            uint32_t base_;
            uint32_t size_;
            uint32_t index_;
        };

        void ExecuteIndirect(uint32_t address, uint32_t sizeDwords);

        constexpr uint32_t kScratchUmsk = 0x01DC;
        constexpr uint32_t kScratchAddr = 0x01DD;
        constexpr uint32_t kScratchReg0 = 0x0578;
        constexpr uint32_t kCoherStatusHost = 0x0A31;

        // Register index for a SET_CONSTANT / LOAD_ALU_CONSTANT "type << 16 | offset" dword.
        uint32_t ConstantIndex(uint32_t offsetType)
        {
            const uint32_t offset = offsetType & 0x7FF;
            switch ((offsetType >> 16) & 0xFF)
            {
            case 0: return 0x4000 + offset;  // ALU (float) constants
            case 1: return 0x4800 + offset;  // fetch constants
            case 2: return 0x4900 + offset;  // bool constants
            case 3: return 0x4908 + offset;  // loop constants
            default: return 0x2000 + offset; // ordinary registers
            }
        }

        // Register writes from the command stream, with their side effects.
        void WriteRegisterFromStream(uint32_t index, uint32_t value)
        {
            if (index >= kScratchReg0 && index < kScratchReg0 + 8)
            {
                // Scratch registers enabled in SCRATCH_UMSK are mirrored to memory at
                // SCRATCH_ADDR (big-endian); Direct3D uses this for fences.
                const uint32_t slot = index - kScratchReg0;
                if (ReadRegister(kScratchUmsk) & (1u << slot))
                    Store<uint32_t>(PhysicalToGuest(ReadRegister(kScratchAddr) + slot * 4), value);
            }
            else if (index == kCoherStatusHost)
            {
                // Cache flush/invalidate requests complete immediately: clear the busy bit.
                value &= ~0x80000000u;
            }
            WriteRegister(index, value);
        }

        // Packet statistics, printed every 2 seconds when HC09_GPU_STATS is set.
        const bool g_statsEnabled = std::getenv("HC09_GPU_STATS") != nullptr;
        uint32_t g_opcodeCounts[128];
        uint32_t g_type0Writes = 0;
        uint64_t g_dwordsConsumed = 0;
        uint32_t g_statsSwapBase = 0;
        auto g_statsStart = std::chrono::steady_clock::now();

        // Command processor time split (HC09_GPU_STATS): renderer draws, swaps/presents, and
        // idle (waiting for the title to submit). The rest is packet processing and waits.
        std::chrono::steady_clock::duration g_drawTime{}, g_swapTime{}, g_idleTime{}, g_waitTime{};
        // WAIT_REG_MEM time by polled address (register index or memory address).
        std::map<uint32_t, std::chrono::steady_clock::duration> g_waitByAddress;

        void ReportStats()
        {
            if (!g_statsEnabled)
                return;
            const auto now = std::chrono::steady_clock::now();
            const double seconds = std::chrono::duration<double>(now - g_statsStart).count();
            if (seconds < 2.0)
                return;
            const uint32_t swaps = g_swapCount.load();
            std::fprintf(stderr, "[gpu] %.1f swaps/s (total %u), %llu dwords, rptr %u wptr %u, %u register writes, type-3:",
                (swaps - g_statsSwapBase) / seconds, swaps, (unsigned long long)g_dwordsConsumed, g_readPointer,
                ReadRegister(reg::kCpRbWptr), g_type0Writes);
            for (uint32_t op = 0; op < 128; op++)
                if (g_opcodeCounts[op])
                    std::fprintf(stderr, " %02X:%u", op, g_opcodeCounts[op]);
            auto share = [&](std::chrono::steady_clock::duration d) {
                return 100.0 * std::chrono::duration<double>(d).count() / seconds;
            };
            std::fprintf(stderr, " | draw %.0f%% swap %.0f%% idle %.0f%% wait %.0f%%", share(g_drawTime), share(g_swapTime),
                share(g_idleTime), share(g_waitTime));
            for (const auto& [address, time] : g_waitByAddress)
                if (share(time) >= 1.0)
                    std::fprintf(stderr, " [0x%X %.0f%%]", address, share(time));
            std::fprintf(stderr, "\n");
            g_drawTime = g_swapTime = g_idleTime = g_waitTime = {};
            g_waitByAddress.clear();
            std::memset(g_opcodeCounts, 0, sizeof(g_opcodeCounts));
            g_type0Writes = 0;
            g_dwordsConsumed = 0;
            g_statsSwapBase = swaps;
            g_statsStart = now;
        }

        void ExecuteType3(Reader& reader, uint32_t opcode, uint32_t count)
        {
            g_opcodeCounts[opcode]++;
            if (IsDumpFrame())
            {
                Reader peek = reader;
                std::fprintf(stderr, "[packet] type3 op 0x%02X, %u dwords:", opcode, count);
                for (uint32_t i = 0; i < count && i < 4; i++)
                    std::fprintf(stderr, " %08X", peek.Read());
                std::fprintf(stderr, "\n");
            }
            switch (opcode)
            {
            case pm4::kIndirectBuffer:
            case pm4::kIndirectBufferPfd:
            {
                const uint32_t address = reader.Read();
                const uint32_t size = reader.Read() & 0xFFFFF;
                reader.Skip(count - 2);
                static int indirectTraced = 0;
                if (g_tracingFences && reader.size() == g_ringSizeDwords && indirectTraced++ < 600)
                    std::fprintf(stderr, "[ib] ring dword %u: indirect 0x%08X x%u (wptr %u)%c", reader.index(), address, size,
                        ReadRegister(reg::kCpRbWptr), 10);
                ExecuteIndirect(address, size);
                break;
            }
            case pm4::kSetBinMask:
            case pm4::kSetBinSelect:
            {
                const uint64_t high = reader.Read(), low = reader.Read();
                reader.Skip(count - 2);
                (opcode == pm4::kSetBinMask ? g_binMask : g_binSelect) = (high << 32) | low;
                break;
            }
            case pm4::kSetBinMaskLo:
            case pm4::kSetBinMaskHi:
            case pm4::kSetBinSelectLo:
            case pm4::kSetBinSelectHi:
            {
                const uint64_t value = reader.Read();
                reader.Skip(count - 1);
                uint64_t& target = (opcode == pm4::kSetBinMaskLo || opcode == pm4::kSetBinMaskHi) ? g_binMask : g_binSelect;
                if (opcode == pm4::kSetBinMaskLo || opcode == pm4::kSetBinSelectLo)
                    target = (target & 0xFFFFFFFF00000000ull) | value;
                else
                    target = (target & 0x00000000FFFFFFFFull) | (value << 32);
                break;
            }
            case pm4::kWaitRegMem:
            {
                const uint32_t info = reader.Read(), pollAddress = reader.Read();
                const uint32_t reference = reader.Read(), mask = reader.Read();
                reader.Skip(count - 4);  // wait interval
                const bool memory = (info & 0x10) != 0;
                {
                    // HC09_TRACE_WAITS=1: log each distinct wait (info, address, reference, mask) once.
                    static const bool traceWaits = std::getenv("HC09_TRACE_WAITS") != nullptr;
                    static std::mutex waitsMutex;
                    static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>> seenWaits;
                    if (traceWaits)
                    {
                        std::lock_guard lock(waitsMutex);
                        if (seenWaits.insert({ info, pollAddress, reference, mask }).second)
                            std::fprintf(stderr, "[wait] info 0x%X %s 0x%08X ref 0x%X mask 0x%X (now 0x%X)%c", info,
                                memory ? "memory" : "register", pollAddress, reference, mask,
                                memory ? GpuRead(pollAddress) : ReadRegister(pollAddress), 10);
                    }
                }
                const auto waitStart = std::chrono::steady_clock::now();
                struct WaitTimer
                {
                    std::chrono::steady_clock::time_point start;
                    uint32_t address;
                    ~WaitTimer()
                    {
                        const auto spent = std::chrono::steady_clock::now() - start;
                        g_waitTime += spent;
                        g_waitByAddress[address] += spent;
                    }
                } waitTimer{ waitStart, pollAddress };
                bool reported = false;
                for (int spins = 0;; spins++)
                {
                    const uint32_t value = memory ? GpuRead(pollAddress) : ReadRegister(pollAddress);
                    if (CompareFunction(info, value & mask, reference))
                        break;
                    if (!reported && std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(1))
                    {
                        reported = true;
                        std::fprintf(stderr, "[gpu] WAIT_REG_MEM stalled: %s 0x%X, function %u, ref 0x%X, mask 0x%X, value 0x%X\n",
                            memory ? "memory" : "register", pollAddress, info & 7, reference, mask, value);
                    }
                    // The CPU side usually answers within microseconds: yield for a while before
                    // sleeping (a sleep costs about a millisecond even with 1 ms timer resolution).
                    if (spins > 4000)
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    else
                        std::this_thread::yield();
                }
                break;
            }
            case pm4::kRegRmw:
            {
                const uint32_t info = reader.Read(), andMask = reader.Read(), orMask = reader.Read();
                reader.Skip(count - 3);
                const uint32_t index = info & 0x1FFF;
                WriteRegister(index, (ReadRegister(index) & andMask) | orMask);
                break;
            }
            case pm4::kCondWrite:
            {
                const uint32_t info = reader.Read(), pollAddress = reader.Read();
                const uint32_t reference = reader.Read(), mask = reader.Read();
                const uint32_t writeAddress = reader.Read(), writeData = reader.Read();
                reader.Skip(count - 6);
                const uint32_t value = (info & 0x10) ? GpuRead(pollAddress) : ReadRegister(pollAddress);
                if (CompareFunction(info, value & mask, reference))
                {
                    if (info & 0x100)
                        GpuWrite(writeAddress, writeData);
                    else
                        WriteRegister(writeAddress, writeData);
                }
                break;
            }
            case pm4::kMemWrite:
            {
                uint32_t address = reader.Read();
                for (uint32_t i = 1; i < count; i++, address += 4)
                    GpuWrite(address, reader.Read());
                break;
            }
            case pm4::kEventWriteShd:
            {
                // (initiator, address, value); bit 31 of the initiator writes the swap count
                const uint32_t initiator = reader.Read(), address = reader.Read(), value = reader.Read();
                reader.Skip(count - 3);
                // HC09_TRACE_FENCES=<hex value>: once a fence value at or above it is written, log
                // the next 300 fence writes.
                static const uint32_t traceFencesFrom = std::getenv("HC09_TRACE_FENCES")
                    ? uint32_t(std::strtoul(std::getenv("HC09_TRACE_FENCES"), nullptr, 16)) : 0xFFFFFFFF;
                static int fencesTraced = 0;
                if (!(initiator >> 31) && value >= traceFencesFrom && value < traceFencesFrom + 0x1000)
                    g_tracingFences = true;
                if (g_tracingFences && fencesTraced < 600)
                {
                    fencesTraced++;
                    std::fprintf(stderr, "[fence] 0x%08X <- 0x%08X at dword %u of %s, ring rptr %u wptr %u\n",
                        address, value, reader.index(), reader.size() == g_ringSizeDwords ? "ring" : "indirect",
                        g_readPointer, ReadRegister(reg::kCpRbWptr));
                }
                GpuWrite(address, (initiator >> 31) ? g_swapCount.load() : value);
                break;
            }
            case pm4::kEventWriteExt:
            {
                // Screen extents (min/max x/y in 8-pixel units, min/max z) as u16s.
                reader.Read();
                const uint32_t address = reader.Read();
                reader.Skip(count - 2);
                // Six big-endian u16s regardless of the address' swap bits; D3D turns these into
                // predicated-tiling bin masks, so they must cover every tile.
                const uint16_t extents[6] = { 0, 1280 / 8, 0, 720 / 8, 0, 1 };
                uint8_t* out = GuestToHost(PhysicalToGuest(address & ~3u));
                for (int i = 0; i < 6; i++)
                {
                    out[i * 2] = uint8_t(extents[i] >> 8);
                    out[i * 2 + 1] = uint8_t(extents[i]);
                }
                break;
            }
            case pm4::kInterrupt:
            {
                // One CP interrupt per hardware thread in the CPU mask.
                const uint32_t cpuMask = reader.Read();
                reader.Skip(count - 1);
                uint64_t last = 0;
                for (uint32_t cpu = 0; cpu < 6; cpu++)
                    if (cpuMask & (1u << cpu))
                        last = QueueInterrupt(1, cpu);
                if (last)
                    WaitForInterrupt(last);
                break;
            }
            case pm4::kImLoad:
            {
                // (type | address, start << 16 | size_dwords): microcode in memory
                const uint32_t typeAddress = reader.Read(), startSize = reader.Read();
                reader.Skip(count - 2);
                const uint32_t address = PhysicalToGuest(typeAddress & ~3u);
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    std::fprintf(stderr, "[gpu] IM_LOAD type/address 0x%08X start/size 0x%08X (packet %u dwords)\n",
                        typeAddress, startSize, count);
                }
                std::vector<uint32_t> dwords(startSize & 0xFFFF);
                for (uint32_t i = 0; i < dwords.size(); i++)
                    dwords[i] = Load<uint32_t>(address + i * 4);
                LoadShader(ShaderType(typeAddress & 3), std::move(dwords));
                break;
            }
            case pm4::kImLoadImmediate:
            {
                // (type, start << 16 | size_dwords, microcode...)
                const uint32_t type = reader.Read(), startSize = reader.Read();
                const uint32_t size = std::min(startSize & 0xFFFF, count - 2);
                static std::vector<uint64_t> seen;
                const uint64_t key = (uint64_t(type) << 32) | startSize;
                if (std::find(seen.begin(), seen.end(), key) == seen.end())
                {
                    seen.push_back(key);
                    std::fprintf(stderr, "[gpu] IM_LOAD_IMMEDIATE type 0x%X start/size 0x%08X (packet %u dwords)\n",
                        type, startSize, count);
                }
                std::vector<uint32_t> dwords(size);
                for (uint32_t i = 0; i < size; i++)
                    dwords[i] = reader.Read();
                reader.Skip(count - 2 - size);
                LoadShader(ShaderType(type & 3), std::move(dwords));
                break;
            }
            case pm4::kLoadAluConstant:
            {
                // (address, type << 16 | offset, size_dwords): copy constants from memory
                const uint32_t address = PhysicalToGuest(reader.Read() & 0x3FFFFFFF);
                const uint32_t index = ConstantIndex(reader.Read());
                const uint32_t size = reader.Read() & 0xFFF;
                reader.Skip(count - 3);
                for (uint32_t i = 0; i < size; i++)
                    WriteRegister(index + i, Load<uint32_t>(address + i * 4));
                break;
            }
            case pm4::kSetConstant:
            {
                // (type << 16 | offset, values...)
                const uint32_t index = ConstantIndex(reader.Read());
                for (uint32_t i = 1; i < count; i++)
                    WriteRegisterFromStream(index + i - 1, reader.Read());
                break;
            }
            case pm4::kSetConstant2:
            {
                // (register index, values...)
                const uint32_t index = reader.Read() & 0xFFFF;
                for (uint32_t i = 1; i < count; i++)
                    WriteRegisterFromStream(index + i - 1, reader.Read());
                break;
            }
            case pm4::kDrawIndx:
            case pm4::kDrawIndx2:
            {
                // DRAW_INDX: (viz query, VGT_DRAW_INITIATOR, [index base, index size])
                // DRAW_INDX_2: (VGT_DRAW_INITIATOR, [immediate indices...])
                uint32_t consumed = 0;
                auto next = [&] { consumed++; return reader.Read(); };
                if (opcode == pm4::kDrawIndx)
                    next();
                const uint32_t initiator = next();
                DrawInfo draw{};
                draw.primitive = PrimitiveType(initiator & 0x3F);
                draw.source = IndexSource((initiator >> 6) & 3);
                draw.index32 = (initiator >> 11) & 1;
                draw.indexCount = initiator >> 16;
                std::vector<uint32_t> immediate;
                if (draw.source == IndexSource::Dma && opcode == pm4::kDrawIndx)
                {
                    draw.indexAddress = PhysicalToGuest(next());
                    draw.indexEndian = next() >> 30;
                }
                else if (draw.source == IndexSource::Immediate)
                {
                    while (consumed < count)
                        immediate.push_back(next());
                    draw.immediateIndices = immediate.data();
                    draw.immediateDwords = uint32_t(immediate.size());
                }
                reader.Skip(count - consumed);
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    ExecuteDraw(draw);
                    g_drawTime += std::chrono::steady_clock::now() - t0;
                }
                break;
            }
            case pm4::kXeSwap:
            {
                // ('SWAP', front buffer, width, height, ...), written by VdSwap
                reader.Read();
                const uint32_t frontBuffer = reader.Read(), width = reader.Read(), height = reader.Read();
                reader.Skip(count - 4);
                g_swapCount.fetch_add(1);
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    ExecuteSwap(frontBuffer, width, height);
                    g_swapTime += std::chrono::steady_clock::now() - t0;
                }
                break;
            }
            default:
                // Remaining state packets are not executed yet.
                reader.Skip(count);
                break;
            }
        }

        // Total dwords (header included) of the packet starting with `header`.
        uint32_t PacketLength(uint32_t header)
        {
            switch (header >> 30)
            {
            case 0: return 1 + ((header >> 16) & 0x3FFF) + 1;
            case 1: return 3;
            case 2: return 1;
            default: return 1 + ((header >> 16) & 0x3FFF) + 1;
            }
        }

        // Executes the complete packets between the reader and `end` (a dword index). Stops
        // at a packet that extends past `end`, leaving the reader on its header (the command
        // processor waits for the rest of it, as the hardware does). Returns false then.
        bool ExecutePackets(Reader& reader, uint32_t end)
        {
            while (reader.index() != end)
            {
                const uint32_t available = (end + reader.size() - reader.index()) % reader.size();
                if (PacketLength(reader.Peek()) > available)
                    return false;
                const uint32_t header = reader.Read();
                switch (header >> 30)
                {
                case 0:  // write `count` registers starting at base (or one register repeatedly)
                {
                    const uint32_t baseIndex = header & 0x7FFF;
                    const uint32_t count = ((header >> 16) & 0x3FFF) + 1;
                    if (IsDumpFrame())
                        std::fprintf(stderr, "[packet] type0 registers 0x%04X x%u\n", baseIndex, count);
                    const bool oneRegister = (header >> 15) & 1;
                    g_type0Writes += count;
                    for (uint32_t i = 0; i < count; i++)
                        WriteRegisterFromStream(oneRegister ? baseIndex : baseIndex + i, reader.Read());
                    break;
                }
                case 1:  // two registers
                {
                    const uint32_t first = reader.Read(), second = reader.Read();
                    WriteRegisterFromStream(header & 0x7FF, first);
                    WriteRegisterFromStream((header >> 11) & 0x7FF, second);
                    break;
                }
                case 2:  // padding
                    break;
                case 3:
                    // Predicated rendering draws: targets here are screen-sized, so the first tile
                    // pass (bin select bit 31) renders them over the whole screen and later passes
                    // skip them. (On hardware each pass reaches only its tile, and the bin masks are
                    // refined from screen-extent queries we don't reproduce.) Everything else, resolves
                    // included (copy-mode draws: each tile's copy-and-clear), honours the masks.
                    const uint32_t predicatedOp = (header >> 8) & 0x7F;
                    const bool isDraw = (predicatedOp == pm4::kDrawIndx || predicatedOp == pm4::kDrawIndx2) &&
                        (ReadRegister(0x2208) & 7) != 6;  // RB_MODECONTROL edram mode != copy
                    const bool pass = isDraw ? (!TilingActive() || (g_binSelect & 0x80000000ull) != 0) : (g_binSelect & g_binMask) != 0;
                    if ((header & 1) && !pass)
                    {
                        if (IsDumpFrame())
                            std::fprintf(stderr, "[packet] predicated off: op 0x%02X select %016llX mask %016llX\n", (header >> 8) & 0x7F,
                                (unsigned long long)g_binSelect, (unsigned long long)g_binMask);
                        reader.Skip(((header >> 16) & 0x3FFF) + 1);  // predicated off for this tile
                        break;
                    }
                    ExecuteType3(reader, (header >> 8) & 0x7F, ((header >> 16) & 0x3FFF) + 1);
                    break;
                }
            }
            return true;
        }

        void ExecuteIndirect(uint32_t address, uint32_t sizeDwords)
        {
            Reader reader(PhysicalToGuest(address), sizeDwords + 1, 0);
            if (!ExecutePackets(reader, sizeDwords))
            {
                static int reports = 0;
                if (reports++ < 10)
                    std::fprintf(stderr, "[gpu] indirect buffer 0x%08X (%u dwords) ends inside a packet at %u (header 0x%08X)\n",
                        address, sizeDwords, reader.index(), reader.Peek());
            }
        }

        void CommandProcessorThread()
        {
            debug::RegisterThread(0xC0);  // shows up as 0xC0 in HC09_THREAD_STATS
            uint32_t idleSpins = 0;
            for (;;)
            {
                ReportStats();
                const uint32_t writePointer = ReadRegister(reg::kCpRbWptr) % g_ringSizeDwords;
                if (writePointer == g_readPointer)
                {
                    // Idle: yield for a while after the last submission (the title kicks often),
                    // then sleep.
                    const auto t0 = std::chrono::steady_clock::now();
                    if (++idleSpins > 4000)
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                    else
                        std::this_thread::yield();
                    g_idleTime += std::chrono::steady_clock::now() - t0;
                    continue;
                }
                idleSpins = 0;
                Reader reader(g_ringBase, g_ringSizeDwords, g_readPointer);
                const uint32_t pending = (writePointer + g_ringSizeDwords - g_readPointer) % g_ringSizeDwords;
                if (pending > g_ringSizeDwords - 64)
                {
                    static auto lastReport = std::chrono::steady_clock::time_point{};
                    if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(1))
                    {
                        lastReport = std::chrono::steady_clock::now();
                        std::fprintf(stderr, "[gpu] near-full ring batch: rptr %u -> wptr %u (%u dwords pending)\n",
                            g_readPointer, writePointer, pending);
                    }
                }
                // HC09_TRACE_RING=<seconds>: from that time on, log every batch (100 of them).
                static const int traceRingAfter = std::getenv("HC09_TRACE_RING") ? std::atoi(std::getenv("HC09_TRACE_RING")) : -1;
                static const auto processStart = std::chrono::steady_clock::now();
                static int traced = 0;
                if (traceRingAfter >= 0 && traced < 100 &&
                    std::chrono::steady_clock::now() - processStart > std::chrono::seconds(traceRingAfter))
                {
                    traced++;
                    std::fprintf(stderr, "[ring] %u -> %u (%u dwords)\n", g_readPointer, writePointer, pending);
                }
                g_dwordsConsumed += pending;
                if (!ExecutePackets(reader, writePointer) && reader.index() == g_readPointer)
                {
                    // Nothing complete yet: wait for the title to write the rest of the packet.
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                    continue;
                }
                g_readPointer = reader.index();
                WriteRegister(reg::kCpRbRptr, g_readPointer);
                if (g_rptrWriteBack)
                    Store<uint32_t>(g_rptrWriteBack, g_readPointer);
            }
        }
    }

    uint32_t ReadRegister(uint32_t index)
    {
        return Load<uint32_t>(kRegisterBase + (index % kRegisterCount) * 4);
    }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        Store<uint32_t>(kRegisterBase + (index % kRegisterCount) * 4, value);
    }

    // Tiling selects individual tiles through the low 32 bits of the bin select; all-ones
    // there (the high half is usually 0) means no tiling.
    bool TilingActive()
    {
        return uint32_t(g_binSelect) != 0xFFFFFFFFu;
    }

    bool InFirstTilePass()
    {
        return TilingActive() && (g_binSelect & 0x80000000ull) != 0;
    }

    bool FenceTraceActive()
    {
        return g_tracingFences;
    }

    uint32_t PhysicalToGuest(uint32_t address)
    {
        return address < 0x20000000 ? kPhysicalHeapBase + address : address;
    }

    void InitializeRingBuffer(uint32_t address, uint32_t sizeLog2)
    {
        g_ringBase = PhysicalToGuest(address);
        g_ringSizeDwords = (1u << (sizeLog2 + 3)) / 4;
        g_readPointer = 0;
        std::fprintf(stderr, "[gpu] ring buffer at 0x%08X, %u dwords\n", g_ringBase, g_ringSizeDwords);
        std::thread(CommandProcessorThread).detach();
    }

    void EnableReadPointerWriteBack(uint32_t address, uint32_t blockSizeLog2)
    {
        (void)blockSizeLog2;
        g_rptrWriteBack = PhysicalToGuest(address);
    }

    void SetInterruptCallback(uint32_t callback, uint32_t userData)
    {
        bool first;
        {
            std::lock_guard lock(g_interruptMutex);
            first = g_interruptCallback == 0;
            g_interruptCallback = callback;
            g_interruptUserData = userData;
        }
        std::fprintf(stderr, "[gpu] interrupt callback 0x%08X (data 0x%08X)\n", callback, userData);
        if (first && callback)
        {
            std::thread(InterruptThread, CreateGuestThread(0x10000)).detach();
            std::thread(VblankThread).detach();
        }
    }

    uint32_t SwapCount()
    {
        return g_swapCount.load();
    }
}
