#pragma once

// The game's own pictures, read from the disc image: the portraits of its portrait libraries (by portrait id)
// and the face textures of the players' 3D heads (by head name, PGHE's PFCG).
//
// The disc's archives hold smaller archives ("BGFA1.05"): a header, a directory and the files, each
// zlib-compressed. The portrait libraries are archives of unnamed files whose key is the portrait id; a head's
// archive names its files (<head>_COL_HIGH is the face). A picture is an XPR2 package holding one texture,
// tiled and block-compressed as the Xbox 360 stores it (tools/roster_import/xpr.py does the same in Python).

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace hc09::editor
{
    struct Pixels
    {
        int width = 0, height = 0;
        std::vector<uint8_t> bgra;  // straight alpha
    };

    class DiscImages
    {
    public:
        ~DiscImages();
        // `cache`: where the places found are kept for the same image (finding them reads the whole disc).
        bool Open(const std::filesystem::path& iso, const std::filesystem::path& cache);
        bool Portrait(bool coach, int id, Pixels& out) const;
        bool Head(const std::string& name, Pixels& out) const;

    private:
        struct File
        {
            uint64_t offset = 0;
            uint32_t size = 0;
        };
        bool Decode(const File& file, Pixels& out) const;

        HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
        const uint8_t* data_ = nullptr;
        uint64_t size_ = 0;
        std::map<int, File> players_, coaches_;
        std::map<std::string, File> heads_;
    };

    // The picture's rectangle (fractions of its size) scaled to `size` x `size` over `background`.
    HBITMAP ToBitmap(const Pixels& pixels, int size, COLORREF background, float left = 0, float top = 0, float right = 1, float bottom = 1);
}
