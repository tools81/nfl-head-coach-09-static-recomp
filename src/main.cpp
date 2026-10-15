// Host entry point: maps the guest address space, loads the decrypted image, and runs
// the recompiled game from its XEX entry point.

#include <ppc_recomp_shared.h>

#include <windows.h>
#include <shellapi.h>
#include <timeapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "memory.h"
#include "common/settings.h"
#include "common/updates.h"
#include "debug/stack_dump.h"
#include "kernel/guest.h"
#include "kernel/heap.h"
#include "kernel/threads.h"
#include "kernel/variables.h"
#include "kernel/vfs.h"
#include "kernel/xex.h"

namespace hc09
{
    uint8_t* g_base = nullptr;

    void ReportStubCall(const char* name, const char* library, const PPCContext& ctx, bool& reported)
    {
        if (reported)
            return;
        reported = true;
        std::fprintf(stderr, "[stub] %s (%s) called from 0x%08X\n", name, library, uint32_t(ctx.lr));
    }
}

namespace
{
    constexpr uint32_t kEntryPoint = 0x828A9CB0;
    constexpr uint32_t kMainStackSize = 0x100000;  // XEX asks for 0x20000; leave headroom

    bool LoadImage(const char* path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const auto size = size_t(file.tellg());
        if (size != PPC_IMAGE_SIZE)
        {
            std::fprintf(stderr, "%s: expected 0x%llX bytes, got 0x%zX\n", path, PPC_IMAGE_SIZE, size);
            return false;
        }
        file.seekg(0);
        file.read(reinterpret_cast<char*>(hc09::g_base + PPC_IMAGE_BASE), std::streamsize(size));
        return bool(file);
    }

    // Target of every indirect call to code the recompiler did not emit as a function.
    // Indirect calls go through CTR (bctrl), so CTR holds the missing guest address.
    PPC_FUNC(MissingFunction)
    {
        (void)base;
        hc09::GuestFatal(ctx, "indirect call to 0x%08X, which is not a recompiled function (add it to config/HC09.base.toml)",
            ctx.ctr.u32);
    }

    void PopulateFunctionTable()
    {
        for (uint64_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
            PPC_LOOKUP_FUNC(hc09::g_base, address) = MissingFunction;
        for (auto* mapping = PPCFuncMappings; mapping->host != nullptr; ++mapping)
            PPC_LOOKUP_FUNC(hc09::g_base, mapping->guest) = mapping->host;
    }

    LONG CALLBACK ReportGuestFault(EXCEPTION_POINTERS* info)
    {
        const auto* record = info->ExceptionRecord;
        if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2)
        {
            const auto address = uintptr_t(record->ExceptionInformation[1]);
            const auto guest = uintptr_t(hc09::g_base);
            if (address >= guest && address < guest + PPC_MEMORY_SIZE)
                std::fprintf(stderr, "[fault] %s guest address 0x%08llX\n",
                    record->ExceptionInformation[0] ? "write to" : "read from", uint64_t(address - guest));
            else
                std::fprintf(stderr, "[fault] host address %p\n", reinterpret_cast<void*>(address));

            // A jump to 0 (an indirect guest call with no recompiled target) leaves the
            // caller's return address at the top of the stack; unwind from there.
            CONTEXT context = *info->ContextRecord;
            if (context.Rip == 0)
            {
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
                std::fprintf(stderr, "[fault] jump to address 0 (missing indirect call target?)\n");
            }
            static std::atomic<bool> reported{ false };
            if (!reported.exchange(true))
                hc09::debug::PrintStackFromContext(&context);
        }
        else if (record->ExceptionCode == EXCEPTION_BREAKPOINT)
        {
            // PowerPC trap instructions (debug asserts, __debugbreak) are recompiled to host traps.
            static std::atomic<bool> reported{ false };
            if (!reported.exchange(true))
            {
                std::fprintf(stderr, "[fault] guest trap instruction (assert/debug break)\n");
                hc09::debug::PrintStackFromContext(info->ContextRecord);
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // The default data paths (build/..., iso/...) are relative to the project folder. When the
    // executable is started from elsewhere (double-clicked in build/hc09, a shortcut), find the
    // project folder by walking up from the executable's own folder and make it current.
    void FindProjectFolder()
    {
        if (GetFileAttributesW(L"build\\default.pe") != INVALID_FILE_ATTRIBUTES)
            return;
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return;
        std::wstring folder(path, length);
        for (int depth = 0; depth < 6; depth++)
        {
            const size_t slash = folder.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return;
            folder.resize(slash);
            if (GetFileAttributesW((folder + L"\\build\\default.pe").c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                SetCurrentDirectoryW(folder.c_str());
                return;
            }
        }
    }

    // With automatic updates enabled (hc09.ini, set by the installer): if GitHub has a newer
    // release, ask, and on "Yes" start the installer in update mode and return true (the game then
    // exits; the updater rebuilds it).
    bool OfferUpdate()
    {
        const auto tag = hc09::LatestReleaseTag(3000);
        if (!tag || !hc09::IsNewerVersion(*tag, HC09_VERSION))
            return false;
        char message[512];
        std::snprintf(message, sizeof(message),
            "Version %s of the NFL Head Coach 09 recompilation is available (installed: %s).\n\n"
            "Update now? The updater downloads the new source and rebuilds the game, which takes a few minutes.",
            tag->c_str(), HC09_VERSION);
        if (MessageBoxA(nullptr, message, "NFL Head Coach 09", MB_YESNO | MB_ICONINFORMATION) != IDYES)
            return false;
        const std::wstring folder = std::filesystem::current_path().wstring();
        const std::wstring updater = folder + L"\\build\\installer\\HC09Setup.exe";
        const std::wstring arguments = L"--update \"" + folder + L"\"";
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", updater.c_str(), arguments.c_str(), folder.c_str(), SW_SHOWNORMAL)) <= 32)
        {
            MessageBoxA(nullptr, "The updater (build\\installer\\HC09Setup.exe) could not be started. Download the latest "
                "HC09Setup.exe from the project's GitHub releases and run it to update.", "NFL Head Coach 09", MB_OK | MB_ICONWARNING);
            return false;
        }
        return true;
    }

    // Startup failures: printed, and shown in a message box (a double-clicked console window
    // closes before the message can be read).
    int StartupFailure(const char* message)
    {
        std::fprintf(stderr, "%s\n", message);
        MessageBoxA(nullptr, message, "NFL Head Coach 09", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }
}

int main(int argc, char** argv)
{
    std::fprintf(stderr, "[boot] HC09 recomp " HC09_VERSION "\n");
    // 1 ms timer resolution: guest waits and our short sleeps otherwise last ~15 ms.
    timeBeginPeriod(1);
    FindProjectFolder();
    const hc09::Settings settings = hc09::LoadSettings(std::filesystem::current_path());
    if (settings.autoUpdate && OfferUpdate())
        return EXIT_SUCCESS;
    const char* imagePath = argc > 1 ? argv[1] : "build/default.pe";
    const char* xexPath = argc > 2 ? argv[2] : "build/extracted/default.xex";
    // The disc image: the command line, else the installer's setting, else the developer layout.
    const std::filesystem::path isoPath = argc > 3 ? std::filesystem::path(argv[3])
        : !settings.isoPath.empty()             ? std::filesystem::path(settings.isoPath)
                                                : std::filesystem::path("iso/NFL Head Coach 09.iso");
    char message[512];

    if (!hc09::AllocateGuestMemory())
        return StartupFailure("Failed to reserve the 4 GiB guest address space.");
    if (!LoadImage(imagePath))
    {
        std::snprintf(message, sizeof(message),
            "Failed to load %s.\n\nRun HC09.exe from the project folder (or anywhere inside it), "
            "after installing with HC09Setup.exe (or extracting the game with tools/xexdecrypt.py).", imagePath);
        return StartupFailure(message);
    }
    if (!hc09::LoadXexHeader(xexPath))
    {
        std::snprintf(message, sizeof(message), "Failed to load the XEX header from %s.", xexPath);
        return StartupFailure(message);
    }
    if (!hc09::InitializeVfs(isoPath, "build/userdata"))
    {
        std::snprintf(message, sizeof(message), "Failed to open the game disc image:\n%s\n\nRun HC09Setup.exe again to select it.",
            isoPath.string().c_str());
        return StartupFailure(message);
    }
    PopulateFunctionTable();
    hc09::PatchImportSlots();
    AddVectoredExceptionHandler(1, ReportGuestFault);
    hc09::debug::StartWatchdogFromEnvironment();

    PPCContext ctx{};
    hc09::EnterGuestThread(ctx, hc09::CreateGuestThread(kMainStackSize));

    std::fprintf(stderr, "[boot] entering 0x%08X\n", kEntryPoint);
    uint8_t* base = hc09::g_base;  // PPC_CALL_INDIRECT_FUNC expects `ctx` and `base`
    PPC_CALL_INDIRECT_FUNC(kEntryPoint);
    std::fprintf(stderr, "[boot] entry point returned\n");
    return EXIT_SUCCESS;
}
