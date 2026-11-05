// HC09HDPack: makes the HD texture pack on the player's PC from the textures the game has collected (hc09.ini
// [Graphics] HDTextures=on: the Vulkan renderer saves each texture the game loads as build\hashed\<content hash>.png,
// and the game starts this tool when it closes). Each new colour texture is upscaled 4x with Real-ESRGAN
// (realesrgan-ncnn-vulkan, MIT; its realesrgan-x4plus model, BSD-3), downloaded into thirdparty\realesrgan on first
// use, and saved at twice the game's size (what the replacement loader would keep of it) as
// textures\hd\<content hash>.png, where the game loads it (gpu/texture_replacements.h). Data textures (normal maps,
// masks, noise, flat fills) are left as the game has them: an image upscaler would invent detail in them. Nothing is
// sent anywhere and nothing of the game's goes into the project.
//
// Run from the project folder (the game's working folder): HC09HDPack.exe [--wait <pid>]. --wait first waits for that
// process (the game) to end. Progress goes to the console and build\hdpack.log.

#include <windows.h>
#include <bcrypt.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
    // The Real-ESRGAN release (v0.2.5.0, Windows build) and its SHA-256.
    constexpr const wchar_t* kUpscalerUrl =
        L"https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
    constexpr const char* kUpscalerSha256 = "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d";
    constexpr const wchar_t* kModel = L"realesrgan-x4plus";

    FILE* g_log = nullptr;

    void Log(const char* format, ...)
    {
        char line[1024];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        std::fputs(line, stdout);
        std::fputc('\n', stdout);
        if (g_log)
        {
            std::fputs(line, g_log);
            std::fputc('\n', g_log);
            std::fflush(g_log);
        }
    }

    struct Image
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> rgba;   // straight (not premultiplied) R, G, B, A bytes
    };

    IWICImagingFactory* Wic()
    {
        static ComPtr<IWICImagingFactory> factory = [] {
            ComPtr<IWICImagingFactory> f;
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
            return f;
        }();
        return factory.Get();
    }

    bool Load(const fs::path& path, Image& out)
    {
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (!Wic() || FAILED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) || FAILED(Wic()->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&out.width, &out.height)))
            return false;
        out.rgba.resize(size_t(out.width) * out.height * 4);
        return SUCCEEDED(converter->CopyPixels(nullptr, out.width * 4, UINT(out.rgba.size()), out.rgba.data()));
    }

    bool Save(const fs::path& path, const Image& image)
    {
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        // The PNG encoder takes B, G, R, A.
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        std::vector<uint8_t> bgra = image.rgba;
        for (size_t i = 0; i + 3 < bgra.size(); i += 4)
            std::swap(bgra[i], bgra[i + 2]);
        const fs::path temporary = fs::path(path).concat(L".tmp");
        bool ok = Wic() && SUCCEEDED(Wic()->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(image.width, image.height)) &&
            SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(frame->WritePixels(image.height, image.width * 4, UINT(bgra.size()), bgra.data())) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        std::error_code ec;
        if (ok)
            fs::rename(temporary, path, ec);
        else
            fs::remove(temporary, ec);
        return ok && !ec;
    }

    // Halves an image (2x2 boxes; colours weighted by alpha, so transparent texels do not darken edges).
    Image Half(const Image& in)
    {
        Image out;
        out.width = std::max(1u, in.width / 2);
        out.height = std::max(1u, in.height / 2);
        out.rgba.resize(size_t(out.width) * out.height * 4);
        for (uint32_t y = 0; y < out.height; y++)
            for (uint32_t x = 0; x < out.width; x++)
            {
                uint32_t sum[3] = {}, alpha = 0;
                for (uint32_t dy = 0; dy < 2; dy++)
                    for (uint32_t dx = 0; dx < 2; dx++)
                    {
                        const uint32_t sx = std::min(in.width - 1, x * 2 + dx), sy = std::min(in.height - 1, y * 2 + dy);
                        const uint8_t* p = &in.rgba[(size_t(sy) * in.width + sx) * 4];
                        const uint32_t a = p[3] + 1;   // +1: fully transparent boxes still get a colour
                        for (int c = 0; c < 3; c++)
                            sum[c] += p[c] * a;
                        alpha += a;
                    }
                uint8_t* q = &out.rgba[(size_t(y) * out.width + x) * 4];
                for (int c = 0; c < 3; c++)
                    q[c] = uint8_t((sum[c] + alpha / 2) / alpha);
                q[3] = uint8_t((alpha - 4 + 2) / 4);
            }
        return out;
    }

    // Colour image, or a data texture to leave as it is (why).
    const char* Classify(const Image& image)
    {
        const uint32_t w = image.width, h = image.height;
        if (std::min(w, h) < 32)
            return "small";
        auto at = [&](uint32_t x, uint32_t y) { return &image.rgba[(size_t(y) * w + x) * 4]; };
        double mr = 0, mg = 0, mb = 0, sat = 0, opaque = 0, n = 0;
        std::vector<const uint8_t*> samples;
        for (uint32_t y = 0; y < h; y += std::max(1u, h / 64))
            for (uint32_t x = 0; x < w; x += std::max(1u, w / 64))
                samples.push_back(at(x, y));
        for (const uint8_t* p : samples)
        {
            mr += p[0], mg += p[1], mb += p[2];
            sat += std::max({ p[0], p[1], p[2] }) - std::min({ p[0], p[1], p[2] });
            opaque += p[3] > 16;
            n++;
        }
        mr /= n, mg /= n, mb /= n, sat /= n, opaque /= n;
        double var = 0;
        for (const uint8_t* p : samples)
            var += (p[0] - mr) * (p[0] - mr) + (p[1] - mg) * (p[1] - mg) + (p[2] - mb) * (p[2] - mb);
        var /= n;
        // Tangent-space normal maps: blue-violet.
        if (mb > 150 && mb > mr + 40 && mb > mg + 40)
            return "normal map";
        // Two-channel normal maps (blue empty): olive.
        if (mb < 50 && std::fabs(mr - 128) < 55 && std::fabs(mg - 128) < 55 && sat > 40)
            return "normal map";
        // One-channel maps shown in red or magenta (skin, specular).
        if (mr > 80 && mr > 2.2 * mg)
            return "mask";
        if (opaque < 0.02)
            return "transparent";
        if (var < 60)
            return "flat";
        // Mean difference between neighbouring texels along x (dx 1) or y (dy 1), against the spread of all of them.
        const double sd = std::max(1.0, std::sqrt(var / 3));
        auto neighbours = [&](uint32_t dx, uint32_t dy) {
            double d = 0, m = 0;
            for (uint32_t y = 0; y + dy < h; y += std::max(1u, h / 48))
                for (uint32_t x = 0; x + dx < w; x += std::max(1u, w / 48))
                {
                    const uint8_t* a = at(x, y);
                    const uint8_t* b = at(x + dx, y + dy);
                    d += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
                    m++;
                }
            return m ? d / m / 3 / sd : 0.0;
        };
        const double rx = neighbours(1, 0), ry = neighbours(0, 1);
        // Noise: neighbouring texels differ about as much as any two do.
        if (rx > 0.7)
            return "noise";
        // Strips: detail along one axis only (trims, stripes, and tables the game looks up per row, as the one
        // the jerseys' names are drawn through: smoothed across its rows, the names come out speckled).
        if (std::max(rx, ry) > 0.2 && std::max(rx, ry) > 8 * std::min(rx, ry))
            return "strip";
        return nullptr;
    }

    bool Run(const std::wstring& commandLine, const fs::path& directory, bool quiet)
    {
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        std::wstring line = commandLine;
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, quiet ? CREATE_NO_WINDOW : 0, nullptr,
                directory.empty() ? nullptr : directory.c_str(), &startup, &process))
            return false;
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return code == 0;
    }

    std::wstring Quote(const fs::path& path) { return L"\"" + path.wstring() + L"\""; }

    std::string Sha256(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::string hex;
        if (!file || BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            return hex;
        if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0)
        {
            std::vector<char> buffer(1 << 20);
            while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0)
                BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(file.gcount()), 0);
            uint8_t digest[32];
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
                for (uint8_t b : digest)
                {
                    char two[3];
                    std::snprintf(two, sizeof(two), "%02x", b);
                    hex += two;
                }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return hex;
    }

    // thirdparty\realesrgan\realesrgan-ncnn-vulkan.exe, downloaded and checked on first use.
    fs::path Upscaler(const fs::path& root)
    {
        const fs::path folder = root / L"thirdparty\\realesrgan";
        const fs::path exe = folder / L"realesrgan-ncnn-vulkan.exe";
        if (fs::exists(exe) && fs::exists(folder / L"models" / (std::wstring(kModel) + L".bin")))
            return exe;
        Log("Downloading the Real-ESRGAN upscaler (45 MB) into thirdparty\\realesrgan");
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        std::error_code ec;
        fs::create_directories(folder, ec);
        const fs::path archive = root / L"build\\realesrgan.zip";
        if (!Run(Quote(fs::path(system) / L"curl.exe") + L" -L --fail -sS -o " + Quote(archive) + L" " + kUpscalerUrl, root, true))
        {
            Log("The download failed (no connection to GitHub?).");
            return {};
        }
        if (Sha256(archive) != kUpscalerSha256)
        {
            Log("The download is not the expected file: not used.");
            fs::remove(archive, ec);
            return {};
        }
        if (!Run(Quote(fs::path(system) / L"tar.exe") + L" -xf " + Quote(archive) + L" -C " + Quote(folder), root, true) || !fs::exists(exe))
        {
            Log("Extracting the upscaler failed.");
            return {};
        }
        fs::remove(archive, ec);
        return exe;
    }
}

int wmain(int argc, wchar_t** argv)
{
    // Only one at a time (the game may start one at each exit).
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\HC09HDPack");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    for (int i = 1; i + 1 < argc; i++)
        if (std::wstring(argv[i]) == L"--wait")
            if (HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, DWORD(_wtoi(argv[i + 1]))))
            {
                WaitForSingleObject(game, 60000);
                CloseHandle(game);
            }
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const fs::path root = fs::current_path();
    const fs::path dumps = root / L"build\\hashed", pack = root / L"textures\\hd";
    std::error_code ec;
    fs::create_directories(pack, ec);
    fs::create_directories(root / L"build", ec);
    g_log = _wfopen((root / L"build\\hdpack.log").c_str(), L"a");

    // Textures done before: in the pack, or found to be data (pack\skipped.txt).
    std::set<std::wstring> done;
    const fs::path skippedList = pack / L"skipped.txt";
    {
        std::wifstream list(skippedList);
        for (std::wstring name; std::getline(list, name);)
            if (!name.empty())
                done.insert(name.substr(0, name.find(L' ')));
    }
    std::vector<fs::path> todo;
    if (fs::exists(dumps))
        for (const auto& entry : fs::directory_iterator(dumps, ec))
        {
            const std::wstring name = entry.path().filename().wstring();
            if (entry.path().extension() == L".png" && !done.count(name) && !fs::exists(pack / name))
                todo.push_back(entry.path());
        }
    if (todo.empty())
    {
        Log("HD texture pack: nothing new to upscale.");
        return 0;
    }

    // Sort: colour images to a work folder, data textures to the skipped list.
    const fs::path work = root / L"build\\hdpack", in = work / L"in", out = work / L"out";
    fs::remove_all(work, ec);
    fs::create_directories(in, ec);
    fs::create_directories(out, ec);
    std::wofstream skipped(skippedList, std::ios::app);
    size_t colour = 0, data = 0;
    for (const fs::path& path : todo)
    {
        Image image;
        if (!Load(path, image))
            continue;
        if (const char* why = Classify(image))
        {
            skipped << path.filename().wstring() << L" " << why << L"\n";
            data++;
            continue;
        }
        fs::copy_file(path, in / path.filename(), fs::copy_options::overwrite_existing, ec);
        colour++;
    }
    skipped.close();
    Log("HD texture pack: %zu new textures, %zu to upscale, %zu data textures left as they are", todo.size(), colour, data);
    if (!colour)
        return 0;

    const fs::path upscaler = Upscaler(root);
    if (upscaler.empty())
        return 1;
    Log("Upscaling %zu textures 4x with %ls (the graphics card does the work)", colour, kModel);
    if (!Run(Quote(upscaler) + L" -i " + Quote(in) + L" -o " + Quote(out) + L" -n " + kModel + L" -f png", upscaler.parent_path(), true))
        Log("The upscaler reported an error; keeping what it finished.");

    size_t saved = 0, made = 0;
    for (const auto& entry : fs::directory_iterator(out, ec))
    {
        made++;
        Image image;
        if (!Load(entry.path(), image))
        {
            Log("Could not read the upscaled %ls", entry.path().filename().c_str());
            continue;
        }
        if (Save(pack / entry.path().filename(), Half(image)))
            saved++;
        else
            Log("Could not save %ls", (pack / entry.path().filename()).c_str());
    }
    if (saved == made)
        fs::remove_all(work, ec);
    else
        Log("The upscaler made %zu images; build\\hdpack is kept to look at", made);
    Log("HD texture pack: %zu textures added to textures\\hd (they load from the next start)", saved);
    return 0;
}
