#include "pipeline.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <fstream>
#include <sstream>

#include "common/settings.h"
#include "common/updates.h"
#include "disc.h"
#include "xex.h"

namespace hc09::setup
{
    namespace
    {
        constexpr wchar_t kXenonRecompUrl[] = L"https://github.com/hedge-dev/XenonRecomp";
        // FFmpeg 8.x LGPL shared build (avcodec-62 / avutil-60 / swresample-6, as CMakeLists.txt expects).
        constexpr wchar_t kFfmpegUrl[] =
            L"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip";
        constexpr int kSteps = 8;

        std::wstring Widen(const std::string& text)
        {
            return std::wstring(text.begin(), text.end());
        }

        std::wstring Quote(const std::filesystem::path& path)
        {
            return L"\"" + path.wstring() + L"\"";
        }

        // Tool output is UTF-8 (git, cmake, ninja) or the OEM code page (cmd, cl).
        std::wstring Decode(const std::string& bytes)
        {
            for (const UINT codePage : { UINT(CP_UTF8), UINT(CP_OEMCP) })
            {
                const int length = MultiByteToWideChar(codePage, codePage == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes.data(),
                    int(bytes.size()), nullptr, 0);
                if (length > 0)
                {
                    std::wstring text(length, L'\0');
                    MultiByteToWideChar(codePage, 0, bytes.data(), int(bytes.size()), text.data(), length);
                    return text;
                }
            }
            return Widen(bytes);
        }

        std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
        {
            PWSTR path = nullptr;
            std::filesystem::path result;
            if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
                result = path;
            CoTaskMemFree(path);
            return result;
        }

        std::filesystem::path FindOnPath(const wchar_t* file)
        {
            wchar_t path[MAX_PATH];
            return SearchPathW(nullptr, file, nullptr, MAX_PATH, path, nullptr) ? std::filesystem::path(path) : std::filesystem::path();
        }

        // Runs a command and returns its output (used for vswhere).
        std::string Capture(const std::wstring& commandLine)
        {
            SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
            HANDLE read = nullptr, write = nullptr;
            if (!CreatePipe(&read, &write, &security, 0))
                return {};
            SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW startup{ sizeof(startup) };
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdOutput = write;
            startup.hStdError = write;
            PROCESS_INFORMATION process{};
            std::wstring mutableLine = commandLine;
            std::string output;
            if (CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
            {
                CloseHandle(write);
                write = nullptr;
                char buffer[4096];
                DWORD count = 0;
                while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count)
                    output.append(buffer, count);
                WaitForSingleObject(process.hProcess, 10000);
                CloseHandle(process.hProcess);
                CloseHandle(process.hThread);
            }
            if (write)
                CloseHandle(write);
            CloseHandle(read);
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                output.pop_back();
            return output;
        }

        bool CreateShortcut(const std::filesystem::path& link, const std::filesystem::path& target, const std::filesystem::path& directory,
            const wchar_t* description)
        {
            IShellLinkW* shell = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell))))
                return false;
            shell->SetPath(target.c_str());
            shell->SetWorkingDirectory(directory.c_str());
            shell->SetDescription(description);
            IPersistFile* file = nullptr;
            bool ok = SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file))) && SUCCEEDED(file->Save(link.c_str(), TRUE));
            if (file)
                file->Release();
            shell->Release();
            return ok;
        }
    }

    std::vector<Requirement> CheckRequirements(Toolchain& toolchain)
    {
        std::vector<Requirement> requirements;

        Requirement windows{ L"Windows 10 (1803) or later" };
        const std::filesystem::path system = KnownFolder(FOLDERID_System);
        windows.met = std::filesystem::exists(system / L"curl.exe") && std::filesystem::exists(system / L"tar.exe");
        windows.detail = windows.met ? L"curl and tar are available" : L"curl.exe/tar.exe were not found in System32";
        requirements.push_back(windows);

        Requirement cpu{ L"Processor with AVX" };
        cpu.met = IsProcessorFeaturePresent(39 /* PF_AVX_INSTRUCTIONS_AVAILABLE */) != 0;
        cpu.detail = cpu.met ? L"AVX supported" : L"The recompiled game requires AVX";
        requirements.push_back(cpu);

        Requirement vs{ L"Visual Studio 2022 or later with C++, Clang and CMake tools" };
        const std::filesystem::path vswhere = KnownFolder(FOLDERID_ProgramFilesX86) / L"Microsoft Visual Studio\\Installer\\vswhere.exe";
        if (std::filesystem::exists(vswhere))
        {
            const std::wstring query = Quote(vswhere) + L" -latest -products * -version [17.0,) -property installationPath -requires "
                L"Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang "
                L"Microsoft.VisualStudio.Component.VC.CMake.Project";
            const std::wstring installation = Decode(Capture(query));
            if (!installation.empty() && std::filesystem::exists(std::filesystem::path(installation) / L"VC\\Auxiliary\\Build\\vcvars64.bat"))
            {
                toolchain.vcvars = std::filesystem::path(installation) / L"VC\\Auxiliary\\Build\\vcvars64.bat";
                vs.met = true;
                vs.detail = installation;
            }
            else
            {
                const std::wstring any = Decode(Capture(Quote(vswhere) + L" -latest -products * -version [17.0,) -property installationPath"));
                vs.detail = any.empty() ? L"Not installed"
                    : L"Found " + any + L", but it lacks \"Desktop development with C++\", \"C++ Clang Compiler for Windows\" "
                      L"or \"C++ CMake tools for Windows\" (add them in the Visual Studio Installer)";
            }
        }
        else
        {
            vs.detail = L"Not installed";
        }
        if (!vs.met)
            vs.helpUrl = L"https://visualstudio.microsoft.com/downloads/";
        requirements.push_back(vs);

        Requirement git{ L"Git for Windows" };
        toolchain.git = FindOnPath(L"git.exe");
        if (toolchain.git.empty())
            for (const auto& folder : { KnownFolder(FOLDERID_ProgramFiles), KnownFolder(FOLDERID_UserProgramFiles) })
                if (!folder.empty() && std::filesystem::exists(folder / L"Git\\cmd\\git.exe"))
                    toolchain.git = folder / L"Git\\cmd\\git.exe";
        git.met = !toolchain.git.empty();
        git.detail = git.met ? toolchain.git.wstring() : L"Not installed";
        if (!git.met)
            git.helpUrl = L"https://git-scm.com/download/win";
        requirements.push_back(git);
        return requirements;
    }

    uint64_t FreeSpace(const std::filesystem::path& path)
    {
        std::filesystem::path existing = path;
        std::error_code ec;
        while (!existing.empty() && !std::filesystem::exists(existing, ec))
            existing = existing.parent_path();
        ULARGE_INTEGER available{};
        return GetDiskFreeSpaceExW(existing.c_str(), &available, nullptr, nullptr) ? available.QuadPart : 0;
    }

    bool IsProjectFolder(const std::filesystem::path& folder)
    {
        std::ifstream cmake(folder / L"CMakeLists.txt");
        if (!cmake || !std::filesystem::exists(folder / L".git"))
            return false;
        std::stringstream text;
        text << cmake.rdbuf();
        return text.str().find("project(HC09Recomp") != std::string::npos;
    }

    void Pipeline::Log(const std::wstring& line)
    {
        if (logPath_.empty())
        {
            wchar_t temp[MAX_PATH];
            GetTempPathW(MAX_PATH, temp);
            logPath_ = std::wstring(temp) + L"HC09Setup.log";
            std::ofstream(logPath_, std::ios::trunc);
        }
        std::ofstream file(logPath_, std::ios::app | std::ios::binary);
        const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), int(line.size()), utf8.data(), size, nullptr, nullptr);
        file << utf8 << "\r\n";
        if (callbacks_.log)
            callbacks_.log(line);
    }

    bool Pipeline::Fail(const std::wstring& message)
    {
        Log(L"ERROR: " + message);
        Log(L"The full log is in " + logPath_);
        return false;
    }

    void Pipeline::Cancel()
    {
        cancelled_ = true;
        if (HANDLE process = static_cast<HANDLE>(process_.load()))
            TerminateProcess(process, 1);
    }

    bool Pipeline::Execute(const std::wstring& commandLine, const std::filesystem::path& directory)
    {
        if (cancelled_)
            return false;
        Log(L"> " + commandLine);
        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        HANDLE read = nullptr, write = nullptr;
        if (!CreatePipe(&read, &write, &security, 0))
            return Fail(L"Cannot create a pipe.");
        SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = write;
        startup.hStdError = write;
        PROCESS_INFORMATION process{};
        std::wstring mutableLine = commandLine;
        const std::wstring folder = directory.wstring();
        if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                folder.empty() ? nullptr : folder.c_str(), &startup, &process))
        {
            CloseHandle(read);
            CloseHandle(write);
            return Fail(L"Cannot start: " + commandLine);
        }
        CloseHandle(write);
        process_ = process.hProcess;

        // Stream the output line by line (carriage returns from progress meters end a line too).
        std::string pending;
        char buffer[4096];
        DWORD count = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count)
        {
            pending.append(buffer, count);
            size_t end;
            while ((end = pending.find_first_of("\r\n")) != std::string::npos)
            {
                if (end > 0)
                    Log(L"  " + Decode(pending.substr(0, end)));
                pending.erase(0, end + 1);
            }
        }
        if (!pending.empty())
            Log(L"  " + Decode(pending));
        CloseHandle(read);
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        process_ = nullptr;
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        if (cancelled_)
            return Fail(L"Cancelled.");
        if (exitCode != 0)
            Log(L"  (exit code " + std::to_wstring(exitCode) + L")");
        return exitCode == 0;
    }

    bool Pipeline::ExecuteInVsEnvironment(const std::wstring& commands)
    {
        // vcvars64 looks for vswhere on PATH; the VS installer folder provides it.
        const std::wstring line = L"cmd.exe /d /s /c \"set \"PATH=%ProgramFiles(x86)%\\Microsoft Visual Studio\\Installer;%PATH%\" && call " +
            Quote(toolchain_.vcvars) + L" >nul && " + commands + L"\"";
        return Execute(line, options_.root);
    }

    bool Pipeline::Git(const std::wstring& arguments, const std::filesystem::path& directory)
    {
        // XenonRecomp's submodules nest deeply under .git/modules: allow paths past MAX_PATH.
        return Execute(Quote(toolchain_.git) + L" -c core.longpaths=true " + arguments, directory);
    }

    bool Pipeline::Run()
    {
        struct Step
        {
            const wchar_t* title;
            bool (Pipeline::*run)();
        };
        const Step steps[kSteps] = {
            { L"Getting the source code", &Pipeline::GetSource },
            { L"Getting XenonRecomp", &Pipeline::GetXenonRecomp },
            { L"Getting FFmpeg", &Pipeline::GetFfmpeg },
            { L"Building XenonRecomp", &Pipeline::BuildXenonRecomp },
            { L"Importing the game from the disc image", &Pipeline::ImportGame },
            { L"Recompiling the game", &Pipeline::Recompile },
            { L"Building HC09.exe (this takes a few minutes)", &Pipeline::BuildGame },
            { L"Finishing", &Pipeline::Finish },
        };
        Log(std::wstring(options_.update ? L"Updating " : L"Installing to ") + options_.root.wstring());
        for (int i = 0; i < kSteps; i++)
        {
            if (cancelled_)
                return Fail(L"Cancelled.");
            if (callbacks_.step)
                callbacks_.step(i, kSteps, steps[i].title);
            Log(L"");
            Log(L"== " + std::wstring(steps[i].title) + L" ==");
            if (!(this->*steps[i].run)())
                return false;
        }
        if (callbacks_.step)
            callbacks_.step(kSteps, kSteps, L"Done");
        Log(L"Done.");
        return true;
    }

    bool Pipeline::GetSource()
    {
        const std::filesystem::path& root = options_.root;
        const std::wstring repository = L"https://github.com/" + Widen(kRepository) + L".git";
        if (options_.update)
        {
            if (!IsProjectFolder(root))
                return Fail(root.wstring() + L" does not contain an HC09 installation.");
            const auto tag = LatestReleaseTag(15000);
            if (!tag)
                return Fail(L"The latest release could not be determined (no connection to GitHub?).");
            // Local edits to tracked files would be lost or block the checkout.
            const std::wstring dirty = Decode(Capture(Quote(toolchain_.git) + L" -C " + Quote(root) + L" status --porcelain --untracked-files=no"));
            if (!dirty.empty())
                return Fail(L"The source in " + root.wstring() + L" has local changes; commit or discard them before updating:\n" + dirty);
            return Git(L"fetch --tags --force origin", root) && Git(L"checkout --quiet " + Widen(*tag), root);
        }
        if (IsProjectFolder(root))
        {
            Log(L"Using the source already in " + root.wstring());
            return true;
        }
        std::error_code ec;
        if (std::filesystem::exists(root, ec) && !std::filesystem::is_empty(root, ec))
            return Fail(root.wstring() + L" is not empty. Choose an empty or new folder.");
        if (!Git(L"clone " + repository + L" " + Quote(root), {}))
            return Fail(L"Cloning " + repository + L" failed.");
        if (const auto tag = LatestReleaseTag(15000))
            return Git(L"checkout --quiet " + Widen(*tag), root);
        Log(L"No release has been published yet; using the latest source.");
        return true;
    }

    bool Pipeline::GetXenonRecomp()
    {
        const std::filesystem::path& root = options_.root;
        const std::filesystem::path xenon = root / L"thirdparty\\XenonRecomp";
        const std::filesystem::path patch = root / L"patches\\xenonrecomp.patch";
        std::ifstream pinFile(root / L"patches\\xenonrecomp.commit");
        std::string pin;
        pinFile >> pin;
        if (pin.empty())
            return Fail(L"patches\\xenonrecomp.commit is missing.");

        // Submodules are fetched after moving to the pinned commit, so only their pinned versions download.
        if (!std::filesystem::exists(xenon / L".git") && !Git(L"clone " + std::wstring(kXenonRecompUrl) + L" " + Quote(xenon), root))
            return Fail(L"Cloning XenonRecomp failed.");
        const std::string head = Capture(Quote(toolchain_.git) + L" -C " + Quote(xenon) + L" rev-parse HEAD");
        if (head != pin)
        {
            // Another version (or none) is checked out: drop the old patch and move to the pinned commit.
            if (!Git(L"checkout --quiet -- .", xenon) ||
                (!Git(L"checkout --quiet " + Widen(pin), xenon) && !(Git(L"fetch origin", xenon) && Git(L"checkout --quiet " + Widen(pin), xenon))))
                return Fail(L"Checking out XenonRecomp " + Widen(pin) + L" failed.");
        }
        if (!Git(L"submodule update --init --recursive", xenon))
            return Fail(L"Fetching XenonRecomp's submodules failed.");
        Log(L"Checking whether the HC09 instruction patch is applied");
        if (Git(L"apply --reverse --check " + Quote(patch), xenon))
        {
            Log(L"The patch is already applied.");
            return true;
        }
        if (!Git(L"checkout --quiet -- .", xenon) || !Git(L"apply " + Quote(patch), xenon))
            return Fail(L"Applying patches\\xenonrecomp.patch failed.");
        return true;
    }

    bool Pipeline::GetFfmpeg()
    {
        const std::filesystem::path folder = options_.root / L"thirdparty\\ffmpeg";
        if (std::filesystem::exists(folder / L"lib\\avcodec.lib") && std::filesystem::exists(folder / L"bin"))
        {
            Log(L"FFmpeg is already present.");
            return true;
        }
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        std::filesystem::create_directories(options_.root / L"build", ec);
        const std::filesystem::path archive = options_.root / L"build\\ffmpeg.zip";
        const std::filesystem::path system = KnownFolder(FOLDERID_System);
        if (!Execute(Quote(system / L"curl.exe") + L" -L --fail -sS -o " + Quote(archive) + L" " + kFfmpegUrl, options_.root) ||
            !Execute(Quote(system / L"tar.exe") + L" -xf " + Quote(archive) + L" -C " + Quote(folder) + L" --strip-components 1", options_.root))
            return Fail(L"Downloading or extracting FFmpeg failed.");
        std::filesystem::remove(archive, ec);
        if (!std::filesystem::exists(folder / L"lib\\avcodec.lib"))
            return Fail(L"The FFmpeg download does not contain lib\\avcodec.lib.");
        return true;
    }

    bool Pipeline::BuildXenonRecomp()
    {
        return ExecuteInVsEnvironment(
                   L"cmake -S thirdparty\\XenonRecomp -B build\\xenonrecomp -G Ninja -DCMAKE_BUILD_TYPE=Release "
                   L"-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build build\\xenonrecomp --target XenonRecomp") ||
            Fail(L"Building XenonRecomp failed.");
    }

    bool ImportGameFiles(const std::filesystem::path& iso, const std::filesystem::path& root, std::wstring& error, std::wstring& summary)
    {
        const std::filesystem::path xexPath = root / L"build\\extracted\\default.xex";
        const auto file = FindDiscFile(iso, "default.xex", error);
        if (!file || !ExtractDiscFile(iso, *file, xexPath, error))
        {
            error = L"Reading " + iso.wstring() + L": " + error;
            return false;
        }
        std::ifstream input(xexPath, std::ios::binary);
        const std::vector<uint8_t> xex((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto title = XexTitleId(xex);
        if (!title || *title != kHeadCoach09TitleId)
        {
            error = L"The disc image is not NFL Head Coach 09 (Xbox 360, title 45410841).";
            return false;
        }
        const auto key = LoadRetailKey(root / L"thirdparty\\XenonRecomp\\XenonUtils\\xex.h", error);
        std::vector<uint8_t> image;
        if (!key || !DecryptXex(xex, *key, image, error))
            return false;
        std::ofstream output(root / L"build\\default.pe", std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(image.data()), std::streamsize(image.size()));
        if (!output)
        {
            error = L"Cannot write build\\default.pe.";
            return false;
        }
        summary = L"Extracted default.xex (" + std::to_wstring(xex.size()) + L" bytes) and decrypted it to build\\default.pe (" +
            std::to_wstring(image.size()) + L" bytes)";
        return true;
    }

    bool Pipeline::ImportGame()
    {
        std::wstring error, summary;
        if (!ImportGameFiles(options_.iso, options_.root, error, summary))
            return Fail(error);
        Log(summary);
        return true;
    }

    bool Pipeline::Recompile()
    {
        // Start from an empty ppc/ so no file from an older version survives.
        const std::filesystem::path ppc = options_.root / L"ppc";
        std::error_code ec;
        std::filesystem::remove_all(ppc, ec);
        std::filesystem::create_directories(ppc, ec);
        const std::filesystem::path recompiler = options_.root / L"build\\xenonrecomp\\XenonRecomp\\XenonRecomp.exe";
        return Execute(Quote(recompiler) + L" config\\HC09.toml thirdparty\\XenonRecomp\\XenonUtils\\ppc_context.h", options_.root) ||
            Fail(L"Recompiling the game failed.");
    }

    bool Pipeline::BuildGame()
    {
        if (!ExecuteInVsEnvironment(L"cmake -S . -B build\\hc09 -G Ninja -DCMAKE_BUILD_TYPE=Release "
                                    L"-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl && cmake --build build\\hc09"))
            return Fail(L"Building HC09.exe failed.");
        if (!std::filesystem::exists(options_.root / L"build\\hc09\\HC09.exe"))
            return Fail(L"The build finished without producing build\\hc09\\HC09.exe.");
        return true;
    }

    bool Pipeline::Finish()
    {
        Settings settings = LoadSettings(options_.root);
        if (!options_.iso.empty())
            settings.isoPath = options_.iso.wstring();
        if (!options_.update)
            settings.autoUpdate = options_.autoUpdate;
        if (!SaveSettings(options_.root, settings))
            return Fail(L"Cannot write " + SettingsPath(options_.root).wstring());
        Log(L"Saved " + SettingsPath(options_.root).wstring() + (settings.autoUpdate ? L" (automatic update checks on)" : L""));
        if (options_.update)
            return true;

        // The pipeline runs on a worker thread: the shell link object needs COM on this thread.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const std::filesystem::path game = options_.root / L"build\\hc09\\HC09.exe";
        const wchar_t name[] = L"NFL Head Coach 09 (Recompiled).lnk";
        const wchar_t description[] = L"NFL Head Coach 09 static recompilation";
        bool ok = CreateShortcut(KnownFolder(FOLDERID_Programs) / name, game, options_.root, description);
        if (options_.desktopShortcut)
            ok = CreateShortcut(KnownFolder(FOLDERID_Desktop) / name, game, options_.root, description) && ok;
        if (SUCCEEDED(com))
            CoUninitialize();
        Log(ok ? L"Created shortcuts." : L"Warning: a shortcut could not be created.");
        return true;
    }
}
