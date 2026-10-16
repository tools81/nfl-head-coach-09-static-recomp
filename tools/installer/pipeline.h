#pragma once

// What the installer does: checks the PC for the build toolchain, then fetches the source, builds
// XenonRecomp, imports the game executable from the user's disc image, recompiles it and builds
// HC09.exe. The same steps (minus shortcuts) update an existing install to the latest release.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace hc09::setup
{
    struct Requirement
    {
        std::wstring name;
        bool met = false;
        std::wstring detail;   // what was found, or what is missing
        std::wstring helpUrl;  // where to get it (empty when met)
    };

    struct Toolchain
    {
        std::filesystem::path vcvars;  // VC\Auxiliary\Build\vcvars64.bat
        std::filesystem::path git;
    };

    std::vector<Requirement> CheckRequirements(Toolchain& toolchain);

    struct InstallOptions
    {
        std::filesystem::path root;  // project folder (cloned here unless it already holds the source)
        std::filesystem::path iso;
        bool autoUpdate = false;
        bool desktopShortcut = true;
        bool update = false;         // update an existing install to the latest release
    };

    // Free bytes on the volume holding `path` (or its nearest existing parent).
    uint64_t FreeSpace(const std::filesystem::path& path);
    constexpr uint64_t kRequiredSpace = 6ull << 30;  // source, toolchain builds, recompiled code, objects

    // Extracts default.xex from the disc image into <root>/build/extracted and decrypts it to
    // <root>/build/default.pe (the XEX key comes from <root>/thirdparty/XenonRecomp).
    bool ImportGameFiles(const std::filesystem::path& iso, const std::filesystem::path& root, std::wstring& error, std::wstring& summary);

    // True when `folder` already holds this project's source (a git clone).
    bool IsProjectFolder(const std::filesystem::path& folder);
    // True when `folder` is an installation setup made (a release checkout, not a development
    // branch): running setup on it again updates it to the latest release.
    bool IsReleaseInstall(const std::filesystem::path& folder);

    struct PipelineCallbacks
    {
        std::function<void(int step, int steps, const std::wstring& title)> step;
        std::function<void(const std::wstring& line)> log;
    };

    class Pipeline
    {
    public:
        Pipeline(Toolchain toolchain, InstallOptions options, PipelineCallbacks callbacks)
            : toolchain_(std::move(toolchain)), options_(std::move(options)), callbacks_(std::move(callbacks)) {}

        // Runs every step; false on failure or cancellation (the log says why).
        bool Run();
        void Cancel();

    private:
        bool GetSource();
        bool GetXenonRecomp();
        bool GetFfmpeg();
        bool BuildXenonRecomp();
        bool ImportGame();
        bool Recompile();
        bool BuildGame();
        bool Finish();

        // Runs a command line (in the project folder), streaming its output to the log.
        bool Execute(const std::wstring& commandLine, const std::filesystem::path& directory);
        // Runs commands inside the Visual Studio x64 build environment.
        bool ExecuteInVsEnvironment(const std::wstring& commands);
        bool Git(const std::wstring& arguments, const std::filesystem::path& directory);
        void Log(const std::wstring& line);
        bool Fail(const std::wstring& message);

        Toolchain toolchain_;
        InstallOptions options_;
        PipelineCallbacks callbacks_;
        std::atomic<bool> cancelled_{ false };
        std::atomic<void*> process_{ nullptr };
        std::wstring logPath_;
    };
}
