#include "disc_images.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "disc.h"

namespace hc09::editor
{
    namespace
    {
        uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint64_t Little(const uint8_t* p, uint32_t bytes)
        {
            uint64_t value = 0;
            for (uint32_t i = 0; i < bytes && i < 8; i++)
                value |= uint64_t(p[i]) << (8 * i);
            return value;
        }

        // ---- zlib (RFC 1950/1951) ----

        struct Bits
        {
            const uint8_t* p;
            const uint8_t* end;
            uint32_t hold = 0;
            int count = 0;

            uint32_t Get(int n)
            {
                while (count < n)
                {
                    hold |= uint32_t(p < end ? *p++ : 0) << count;
                    count += 8;
                }
                const uint32_t value = hold & ((1u << n) - 1);
                hold >>= n;
                count -= n;
                return n ? value : 0;
            }
        };

        struct Huffman
        {
            uint16_t counts[16]{}, symbols[288]{};

            void Build(const uint8_t* lengths, int n)
            {
                std::memset(counts, 0, sizeof(counts));
                for (int i = 0; i < n; i++)
                    counts[lengths[i]]++;
                counts[0] = 0;
                uint16_t offsets[16]{};
                for (int i = 1; i < 16; i++)
                    offsets[i] = uint16_t(offsets[i - 1] + counts[i - 1]);
                for (int i = 0; i < n; i++)
                    if (lengths[i])
                        symbols[offsets[lengths[i]]++] = uint16_t(i);
            }

            int Decode(Bits& bits) const
            {
                int code = 0, first = 0, index = 0;
                for (int length = 1; length < 16; length++)
                {
                    code |= int(bits.Get(1));
                    const int count = counts[length];
                    if (code - count < first)
                        return symbols[index + (code - first)];
                    index += count;
                    first = (first + count) << 1;
                    code <<= 1;
                }
                return -1;
            }
        };

        bool Inflate(const uint8_t* source, size_t size, std::vector<uint8_t>& out, size_t most)
        {
            if (size < 6)
                return false;
            Bits bits{ source + 2, source + size };
            static const uint16_t kLengthBase[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
            static const uint8_t kLengthExtra[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
            static const uint16_t kDistanceBase[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                                                      4097, 6145, 8193, 12289, 16385, 24577 };
            static const uint8_t kDistanceExtra[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
            for (bool last = false; !last;)
            {
                last = bits.Get(1) != 0;
                const uint32_t type = bits.Get(2);
                if (type == 0)
                {
                    bits.hold = 0;
                    bits.count = 0;
                    if (bits.p + 4 > bits.end)
                        return false;
                    const uint32_t length = uint32_t(bits.p[0]) | uint32_t(bits.p[1]) << 8;
                    bits.p += 4;
                    if (bits.p + length > bits.end || out.size() + length > most)
                        return false;
                    out.insert(out.end(), bits.p, bits.p + length);
                    bits.p += length;
                    continue;
                }
                if (type == 3)
                    return false;
                Huffman literals, distances;
                uint8_t lengths[320]{};
                if (type == 1)
                {
                    for (int i = 0; i < 288; i++)
                        lengths[i] = uint8_t(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
                    literals.Build(lengths, 288);
                    for (int i = 0; i < 30; i++)
                        lengths[i] = 5;
                    distances.Build(lengths, 30);
                }
                else
                {
                    const int nl = int(bits.Get(5)) + 257, nd = int(bits.Get(5)) + 1, nc = int(bits.Get(4)) + 4;
                    static const uint8_t kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
                    uint8_t codeLengths[19]{};
                    for (int i = 0; i < nc; i++)
                        codeLengths[kOrder[i]] = uint8_t(bits.Get(3));
                    Huffman codes;
                    codes.Build(codeLengths, 19);
                    for (int i = 0; i < nl + nd;)
                    {
                        const int symbol = codes.Decode(bits);
                        if (symbol < 0)
                            return false;
                        if (symbol < 16)
                        {
                            lengths[i++] = uint8_t(symbol);
                            continue;
                        }
                        const uint8_t value = symbol == 16 ? (i ? lengths[i - 1] : 0) : 0;
                        int repeat = symbol == 16 ? 3 + int(bits.Get(2)) : symbol == 17 ? 3 + int(bits.Get(3)) : 11 + int(bits.Get(7));
                        if (i + repeat > nl + nd)
                            return false;
                        while (repeat--)
                            lengths[i++] = value;
                    }
                    literals.Build(lengths, nl);
                    distances.Build(lengths + nl, nd);
                }
                for (;;)
                {
                    const int symbol = literals.Decode(bits);
                    if (symbol < 0 || out.size() > most)
                        return false;
                    if (symbol < 256)
                    {
                        out.push_back(uint8_t(symbol));
                        continue;
                    }
                    if (symbol == 256)
                        break;
                    if (symbol > 285)
                        return false;
                    const int length = kLengthBase[symbol - 257] + int(bits.Get(kLengthExtra[symbol - 257]));
                    const int code = distances.Decode(bits);
                    if (code < 0 || code > 29)
                        return false;
                    const size_t distance = kDistanceBase[code] + bits.Get(kDistanceExtra[code]);
                    if (distance > out.size())
                        return false;
                    for (int i = 0; i < length; i++)
                        out.push_back(out[out.size() - distance]);
                }
                if (bits.p > bits.end)
                    return false;
            }
            return true;
        }

        // ---- textures ----

        // Xenos 2D tiling (Xenia's XGAddress2DTiledOffset): the byte offset of block (x, y).
        uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bytes)
        {
            const uint32_t aligned = (pitch + 31) & ~31u;
            const uint32_t macro = ((x >> 5) + (y >> 5) * (aligned >> 5)) << (log2Bytes + 7);
            const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << log2Bytes;
            const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2Bytes)) + ((y & 1) << 4);
            return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
        }

        void Colour565(uint32_t c, int out[3])
        {
            out[0] = int((c >> 11) & 31) * 255 / 31;
            out[1] = int((c >> 5) & 63) * 255 / 63;
            out[2] = int(c & 31) * 255 / 31;
        }

        // An XPR2 package's first 2D texture (8888, DXT1, DXT3 or DXT5).
        bool DecodeXpr(const std::vector<uint8_t>& blob, Pixels& out)
        {
            if (blob.size() < 32 || std::memcmp(blob.data(), "XPR2", 4) != 0)
                return false;
            const uint32_t header = Be32(&blob[4]), dataSize = Be32(&blob[8]), count = Be32(&blob[12]);
            const size_t dataStart = 12 + size_t(header);
            for (uint32_t i = 0; i < count && 16 + 16 * size_t(i) + 16 <= blob.size(); i++)
            {
                const uint8_t* entry = &blob[16 + 16 * size_t(i)];
                if (std::memcmp(entry, "TX2D", 4) != 0)
                    continue;
                const size_t at = 12 + size_t(Be32(entry + 4)) + 0x1C;
                if (at + 24 > blob.size())
                    return false;
                uint32_t w[6];
                for (int k = 0; k < 6; k++)
                    w[k] = Be32(&blob[at + size_t(k) * 4]);
                const bool tiled = (w[0] >> 31) != 0;
                const uint32_t format = w[1] & 0x3F, endian = (w[1] >> 6) & 3, base = w[1] & 0xFFFFF000u;
                const uint32_t width = (w[2] & 0x1FFF) + 1, height = ((w[2] >> 13) & 0x1FFF) + 1, pitchTexels = ((w[0] >> 22) & 0x1FF) * 32;
                uint32_t unit, block;
                if (format == 0x06)
                    unit = 4, block = 1;
                else if (format == 0x12)
                    unit = 8, block = 4;
                else if (format == 0x13 || format == 0x14)
                    unit = 16, block = 4;
                else
                    return false;
                const uint32_t bw = (width + block - 1) / block, bh = (height + block - 1) / block;
                const uint32_t pitch = std::max(pitchTexels / block, bw), stored = tiled ? (pitch + 31) & ~31u : pitch;
                const uint32_t log2Bytes = unit == 4 ? 2 : unit == 8 ? 3 : 4;
                const size_t available = std::min<size_t>(dataSize, blob.size() - dataStart);
                out.width = int(width);
                out.height = int(height);
                out.bgra.assign(size_t(width) * height * 4, 0);
                for (uint32_t by = 0; by < bh; by++)
                    for (uint32_t bx = 0; bx < bw; bx++)
                    {
                        const size_t offset = size_t(base) + (tiled ? TiledOffset(bx, by, stored, log2Bytes) : (by * stored + bx) * unit);
                        if (offset + unit > available)
                            continue;
                        uint8_t b[16];
                        std::memcpy(b, &blob[dataStart + offset], unit);
                        if (endian == 1)  // 8 in 16
                            for (uint32_t k = 0; k + 1 < unit; k += 2)
                                std::swap(b[k], b[k + 1]);
                        else if (endian == 2)  // 8 in 32
                            for (uint32_t k = 0; k + 3 < unit; k += 4)
                            {
                                std::swap(b[k], b[k + 3]);
                                std::swap(b[k + 1], b[k + 2]);
                            }
                        else if (endian == 3)  // 16 in 32
                            for (uint32_t k = 0; k + 3 < unit; k += 4)
                            {
                                std::swap(b[k], b[k + 2]);
                                std::swap(b[k + 1], b[k + 3]);
                            }
                        if (block == 1)
                        {
                            uint8_t* p = &out.bgra[(size_t(by) * width + bx) * 4];  // A R G B after the swap
                            p[0] = b[3];
                            p[1] = b[2];
                            p[2] = b[1];
                            p[3] = b[0];
                            continue;
                        }
                        const uint8_t* colour = unit == 16 ? b + 8 : b;
                        const uint32_t c0 = colour[0] | colour[1] << 8, c1 = colour[2] | colour[3] << 8;
                        const uint32_t selectors = Le32(colour + 4);
                        int palette[4][3], alphas[4] = { 255, 255, 255, 255 };
                        Colour565(c0, palette[0]);
                        Colour565(c1, palette[1]);
                        const bool four = c0 > c1 || format != 0x12;
                        for (int c = 0; c < 3; c++)
                        {
                            palette[2][c] = four ? (2 * palette[0][c] + palette[1][c]) / 3 : (palette[0][c] + palette[1][c]) / 2;
                            palette[3][c] = four ? (palette[0][c] + 2 * palette[1][c]) / 3 : 0;
                        }
                        if (!four)
                            alphas[3] = 0;
                        int ramp[8]{};
                        uint64_t alphaBits = 0;
                        if (format == 0x14)
                        {
                            ramp[0] = b[0];
                            ramp[1] = b[1];
                            for (int k = 1; k < 7; k++)
                                ramp[k + 1] = b[0] > b[1] ? ((7 - k) * b[0] + k * b[1]) / 7 : k < 5 ? ((5 - k) * b[0] + k * b[1]) / 5 : k == 5 ? 0 : 255;
                            alphaBits = Little(b + 2, 6);
                        }
                        for (uint32_t i = 0; i < 16; i++)
                        {
                            const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                            if (x >= width || y >= height)
                                continue;
                            const uint32_t s = (selectors >> (2 * i)) & 3;
                            uint8_t* p = &out.bgra[(size_t(y) * width + x) * 4];
                            p[0] = uint8_t(palette[s][2]);
                            p[1] = uint8_t(palette[s][1]);
                            p[2] = uint8_t(palette[s][0]);
                            p[3] = uint8_t(format == 0x13 ? ((b[i / 2] >> (4 * (i % 2))) & 15) * 17
                                           : format == 0x14 ? ramp[(alphaBits >> (3 * i)) & 7] : alphas[s]);
                        }
                    }
                return true;
            }
            return false;
        }

        struct Entry
        {
            uint64_t key = 0, offset = 0;
            uint32_t size = 0;
            std::string name;
        };

        // The directory of the archive at `p` (within `end`): false if it is not one.
        bool ReadArchive(const uint8_t* p, const uint8_t* end, std::vector<Entry>& entries)
        {
            if (p + 0x40 > end || std::memcmp(p, "BGFA1.05", 8) != 0)
                return false;
            const uint32_t count = Le32(p + 12), nameBytes = Le32(p + 44);
            const uint64_t directory = Little(p + 16, 8), directorySize = Little(p + 24, 8);
            const uint32_t typeBytes = p[33], keyBytes = p[34], offsetBytes = p[35], packedBytes = p[36], sizeBytes = p[37];
            const uint32_t entryBytes = 1 + typeBytes + keyBytes + offsetBytes + packedBytes + sizeBytes + nameBytes;
            if (!count || count > 100000 || nameBytes > 256 || keyBytes > 8 || offsetBytes > 8 || packedBytes > 4 || directory > 0x1000 ||
                uint64_t(count) * entryBytes > directorySize || p + directory + directorySize > end)
                return false;
            // The entries end the directory (file type tags come first).
            const uint8_t* e = p + directory + directorySize - uint64_t(count) * entryBytes;
            entries.resize(count);
            for (uint32_t i = 0; i < count; i++, e += entryBytes)
            {
                const uint8_t* q = e + 1 + typeBytes;
                entries[i].key = Little(q, keyBytes);
                q += keyBytes;
                entries[i].offset = Little(q, offsetBytes) * 8;  // from the archive's start
                q += offsetBytes;
                entries[i].size = uint32_t(Little(q, packedBytes));
                q += packedBytes + sizeBytes;
                entries[i].name.assign(reinterpret_cast<const char*>(q), strnlen(reinterpret_cast<const char*>(q), nameBytes));
            }
            return true;
        }
    }

    DiscImages::~DiscImages()
    {
        if (data_)
            UnmapViewOfFile(data_);
        if (mapping_)
            CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE)
            CloseHandle(file_);
    }

    bool DiscImages::Open(const std::filesystem::path& iso, const std::filesystem::path& cache)
    {
        file_ = CreateFileW(iso.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER length{};
        GetFileSizeEx(file_, &length);
        size_ = uint64_t(length.QuadPart);
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        data_ = mapping_ ? static_cast<const uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0)) : nullptr;
        if (!data_)
            return false;

        // The cache: the image's size, then "p|c <id> <offset> <size>" and "h <name> <offset> <size>" lines.
        {
            std::ifstream in(cache);
            uint64_t size = 0;
            if (in >> size && size == size_)
            {
                std::string kind;
                while (in >> kind)
                {
                    File f;
                    if (kind == "h")
                    {
                        std::string name;
                        if (in >> name >> f.offset >> f.size)
                            heads_[name] = f;
                    }
                    else
                    {
                        int id = 0;
                        if (in >> id >> f.offset >> f.size)
                            (kind == "p" ? players_ : coaches_)[id] = f;
                    }
                }
                if (!players_.empty() && !coaches_.empty())
                    return true;
            }
        }

        // Every archive in the two archives that hold them: the unnamed ones of 256 x 256 pictures are the
        // portrait libraries (the players' is the largest, the coaches' the one of 700 to 1,000), and the named
        // ones with a <head>_COL_HIGH file are heads.
        struct Library
        {
            std::vector<Entry> entries;
        };
        std::vector<Library> libraries;
        for (const char* name : { "qkl_interface.ast", "qkl_fe2ig.ast" })
        {
            std::wstring ignored;
            const auto found = setup::FindDiscFile(iso, name, ignored);
            if (!found || found->offset + found->size > size_)
                continue;
            const uint8_t* begin = data_ + found->offset;
            const uint8_t* end = begin + found->size;
            for (const uint8_t* p = begin + 8; p + 0x40 < end;)
            {
                p = static_cast<const uint8_t*>(std::memchr(p, 'B', size_t(end - 0x40 - p)));
                if (!p)
                    break;
                std::vector<Entry> entries;
                if (std::memcmp(p, "BGFA1.05", 8) == 0 && ReadArchive(p, end, entries))
                {
                    for (Entry& e : entries)
                        e.offset += uint64_t(p - data_);  // in the image
                    if (entries[0].name.empty() && entries.size() >= 700 && (players_.empty() || coaches_.empty()))
                    {
                        Pixels first;
                        if (Decode({ entries[0].offset, entries[0].size }, first) && first.width == 256 && first.height == 256)
                            libraries.push_back({ entries });
                    }
                    for (const Entry& e : entries)
                    {
                        const size_t suffix = e.name.rfind("_COL_HIGH");
                        if (suffix != std::string::npos && suffix + 9 == e.name.size() && suffix > 0)
                            heads_.emplace(e.name.substr(0, suffix), File{ e.offset, e.size });
                    }
                }
                p++;
            }
            if (libraries.size() >= 2 && players_.empty())
            {
                const auto largest = std::max_element(libraries.begin(), libraries.end(),
                    [](const Library& a, const Library& b) { return a.entries.size() < b.entries.size(); });
                for (const Entry& e : largest->entries)
                    players_[int(e.key & 0xFFFFFFFF)] = { e.offset, e.size };
                for (const Library& library : libraries)
                    if (library.entries.size() < 1000 && coaches_.empty())
                        for (const Entry& e : library.entries)
                            coaches_[int(e.key & 0xFFFFFFFF)] = { e.offset, e.size };
            }
        }
        std::error_code ec;
        std::filesystem::create_directories(cache.parent_path(), ec);
        std::ofstream out(cache, std::ios::trunc);
        out << size_ << '\n';
        for (const auto& [id, f] : players_)
            out << "p " << id << ' ' << f.offset << ' ' << f.size << '\n';
        for (const auto& [id, f] : coaches_)
            out << "c " << id << ' ' << f.offset << ' ' << f.size << '\n';
        for (const auto& [name, f] : heads_)
            out << "h " << name << ' ' << f.offset << ' ' << f.size << '\n';
        return !players_.empty() || !heads_.empty();
    }

    bool DiscImages::Decode(const File& file, Pixels& out) const
    {
        if (!data_ || file.offset + file.size > size_ || file.size < 8)
            return false;
        const uint8_t* p = data_ + file.offset;
        std::vector<uint8_t> blob;
        if (p[0] == 0x78)
        {
            blob.reserve(size_t(file.size) * 4);
            if (!Inflate(p, file.size, blob, 64u << 20))
                return false;
        }
        else
            blob.assign(p, p + file.size);
        return DecodeXpr(blob, out);
    }

    bool DiscImages::Portrait(bool coach, int id, Pixels& out) const
    {
        const auto& library = coach ? coaches_ : players_;
        const auto found = library.find(id);
        return found != library.end() && Decode(found->second, out);
    }

    bool DiscImages::Head(const std::string& name, Pixels& out) const
    {
        const auto found = heads_.find(name);
        return found != heads_.end() && Decode(found->second, out);
    }

    HBITMAP ToBitmap(const Pixels& pixels, int size, COLORREF background, float left, float top, float right, float bottom)
    {
        if (pixels.width <= 0 || pixels.height <= 0)
            return nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = size;
        info.bmiHeader.biHeight = -size;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap)
            return nullptr;
        const int back[3] = { GetBValue(background), GetGValue(background), GetRValue(background) };
        uint8_t* out = static_cast<uint8_t*>(bits);
        const float x0 = left * float(pixels.width), y0 = top * float(pixels.height);
        const float sx = (right - left) * float(pixels.width) / float(size), sy = (bottom - top) * float(pixels.height) / float(size);
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++)
            {
                // The source pixels under this one, averaged (a box filter: the pictures are only made smaller
                // or a little larger).
                const int ax = std::clamp(int(x0 + float(x) * sx), 0, pixels.width - 1), ay = std::clamp(int(y0 + float(y) * sy), 0, pixels.height - 1);
                const int bx = std::clamp(int(x0 + float(x + 1) * sx), ax + 1, pixels.width), by = std::clamp(int(y0 + float(y + 1) * sy), ay + 1, pixels.height);
                uint32_t sum[4]{}, n = 0;
                for (int v = ay; v < by; v++)
                    for (int u = ax; u < bx; u++, n++)
                    {
                        const uint8_t* p = &pixels.bgra[(size_t(v) * pixels.width + u) * 4];
                        for (int c = 0; c < 3; c++)
                            sum[c] += uint32_t(p[c]) * p[3];
                        sum[3] += p[3];
                    }
                uint8_t* o = out + (size_t(y) * size + x) * 4;
                const uint32_t alpha = sum[3] / n;
                for (int c = 0; c < 3; c++)
                    o[c] = uint8_t((sum[c] / n + uint32_t(back[c]) * (255 - alpha)) / 255);
                o[3] = 255;
            }
        return bitmap;
    }
}
