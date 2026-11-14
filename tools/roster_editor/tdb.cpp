#include "tdb.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace hc09::editor
{
    namespace
    {
        constexpr size_t kTableHeader = 36;

        uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
        uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        void PutBe32(uint8_t* p, uint32_t v)
        {
            p[0] = uint8_t(v >> 24);
            p[1] = uint8_t(v >> 16);
            p[2] = uint8_t(v >> 8);
            p[3] = uint8_t(v);
        }
        std::string Tag(const uint8_t* p) { return std::string{ char(p[3]), char(p[2]), char(p[1]), char(p[0]) }; }

        const uint32_t* CrcTable()
        {
            static uint32_t table[256];
            static bool made = false;
            if (!made)
            {
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t c = i << 24;
                    for (int k = 0; k < 8; k++)
                        c = c & 0x80000000 ? (c << 1) ^ 0x04C11DB7 : c << 1;
                    table[i] = c;
                }
                made = true;
            }
            return table;
        }
    }

    // MSB-first CRC-32, initial value 0xFFFFFFFF, no final xor.
    uint32_t TableCrc(const uint8_t* data, size_t size)
    {
        const uint32_t* table = CrcTable();
        uint32_t crc = 0xFFFFFFFF;
        for (size_t i = 0; i < size; i++)
            crc = table[(crc >> 24) ^ data[i]] ^ (crc << 8);
        return crc;
    }

    // The game's checksum of MC02 saves (sub_82F0D220): starts from the complement of the first four bytes
    // and shifts each later byte in.
    uint32_t EaCrc(const uint8_t* data, size_t size)
    {
        if (size < 4)
            return 0;
        const uint32_t* table = CrcTable();
        uint32_t crc = ~Be32(data);
        for (size_t i = 4; i < size; i++)
            crc = table[crc >> 24] ^ ((crc << 8) | data[i]);
        return ~crc;
    }

    const Field* Table::Find(const std::string& field) const
    {
        for (const Field& f : fields)
            if (f.name == field)
                return &f;
        return nullptr;
    }

    uint64_t Table::Bits(size_t base, uint32_t offset, uint32_t bits) const
    {
        const std::vector<uint8_t>& d = db_->Data();
        uint64_t value = 0;
        for (uint32_t i = 0; i < bits; i++)
        {
            const uint32_t bit = offset + i;
            value = value << 1 | ((d[base + bit / 8] >> (7 - bit % 8)) & 1);
        }
        return value;
    }

    void Table::SetBits(size_t base, uint32_t offset, uint32_t bits, uint64_t value)
    {
        std::vector<uint8_t>& d = db_->Data();
        for (uint32_t i = 0; i < bits; i++)
        {
            const uint32_t bit = offset + i;
            const uint8_t mask = uint8_t(1 << (7 - bit % 8));
            if ((value >> (bits - 1 - i)) & 1)
                d[base + bit / 8] |= mask;
            else
                d[base + bit / 8] &= uint8_t(~mask);
        }
    }

    int64_t Table::Get(uint32_t record, const std::string& field) const
    {
        const Field* f = Find(field);
        if (!f || record >= capacity || f->kind == FieldKind::String)
            return 0;
        const uint32_t bits = f->kind == FieldKind::Pooled || f->kind == FieldKind::Compressed ? 32 : std::min<uint32_t>(f->bits, 64);
        const uint64_t value = Bits(dataStart + size_t(record) * recordBytes, f->offset, bits);
        if (f->kind == FieldKind::Signed && bits < 64 && (value >> (bits - 1)) & 1)
            return int64_t(value) - (int64_t(1) << bits);
        return int64_t(value);
    }

    void Table::Set(uint32_t record, const std::string& field, int64_t value)
    {
        const Field* f = Find(field);
        if (!f || record >= capacity || f->kind == FieldKind::String || f->kind == FieldKind::Pooled || f->kind == FieldKind::Compressed)
            return;
        const uint32_t bits = std::min<uint32_t>(f->bits, 64);
        SetBits(dataStart + size_t(record) * recordBytes, f->offset, bits, uint64_t(value));
    }

    uint32_t Table::Max(const std::string& field) const
    {
        const Field* f = Find(field);
        if (!f || f->kind == FieldKind::String)
            return 0;
        return f->bits >= 32 ? 0xFFFFFFFFu : (1u << f->bits) - 1;
    }

    std::string Table::Text(uint32_t record, const std::string& field) const
    {
        const Field* f = Find(field);
        if (!f || record >= capacity)
            return {};
        const std::vector<uint8_t>& d = db_->Data();
        if (f->kind == FieldKind::Pooled || f->kind == FieldKind::Compressed)
        {
            // An offset into the string heap after the records; strings carry their length (one byte, or
            // two when the field can hold more than 255).
            const uint32_t offset = uint32_t(Bits(dataStart + size_t(record) * recordBytes, f->offset, 32));
            if (offset == 0xFFFFFFFF)
                return {};
            const size_t heap = DataEnd();
            size_t at = heap + offset;
            if (at + 2 > d.size())
                return {};
            size_t length;
            if (f->bits / 8 > 255)
            {
                length = Be16(&d[at]);
                at += 2;
            }
            else
                length = d[at++];
            const bool compressed = std::any_of(fields.begin(), fields.end(), [](const Field& x) { return x.kind == FieldKind::Compressed; });
            if (!compressed)
            {
                if (at + length > d.size())
                    return {};
                return std::string(reinterpret_cast<const char*>(&d[at]), length);
            }
            // A table with a compressed field keeps every string Huffman-coded: the heap starts with the
            // tree, two entries a node (the 0 and 1 branches), each either (node, 0) or (0, character);
            // a string is its length, then the code bits, most significant first.
            std::string text;
            size_t bit = 0;
            while (text.size() < length)
            {
                size_t node = 0;
                for (int depth = 0;; depth++)
                {
                    if (at + bit / 8 >= d.size() || depth > 64)
                        return text;
                    const int b = (d[at + bit / 8] >> (7 - bit % 8)) & 1;
                    bit++;
                    const size_t entry = heap + 2 * (2 * node + size_t(b));
                    if (entry + 1 >= d.size())
                        return text;
                    if (d[entry] == 0)
                    {
                        text += char(d[entry + 1]);
                        break;
                    }
                    node = d[entry];
                }
            }
            return text;
        }
        if (f->kind != FieldKind::String)
            return std::to_string(Get(record, field));
        const size_t at = dataStart + size_t(record) * recordBytes + f->offset / 8, size = f->bits / 8;
        const char* text = reinterpret_cast<const char*>(&d[at]);
        return std::string(text, strnlen(text, size));
    }

    void Table::SetText(uint32_t record, const std::string& field, const std::string& text)
    {
        const Field* f = Find(field);
        if (!f || record >= capacity || f->kind != FieldKind::String)
            return;
        const size_t at = dataStart + size_t(record) * recordBytes + f->offset / 8, size = f->bits / 8;
        std::vector<uint8_t>& d = db_->Data();
        std::memset(&d[at], 0, size);
        std::memcpy(&d[at], text.data(), std::min(text.size(), size - 1));
    }

    bool Database::Open(std::vector<uint8_t>* data, size_t offset)
    {
        data_ = data;
        offset_ = offset;
        tables_.clear();
        const std::vector<uint8_t>& d = *data;
        if (offset + 24 > d.size() || std::memcmp(&d[offset], "DB\x00\x08", 4) != 0)
            return false;
        size_ = Be32(&d[offset + 8]);
        const uint32_t count = Be32(&d[offset + 16]);
        if (size_ < 24 || offset + size_ > d.size() || count > 4096 || offset + 24 + size_t(count) * 8 + 4 > d.size())
            return false;
        const size_t directory = offset + 24, directoryEnd = directory + size_t(count) * 8 + 4;
        for (uint32_t i = 0; i < count; i++)
        {
            Table t;
            t.db_ = this;
            t.name = Tag(&d[directory + i * 8]);
            t.start = directoryEnd + Be32(&d[directory + i * 8 + 4]);
            if (t.start + kTableHeader > offset + size_)
                return false;
            const uint8_t* h = &d[t.start];
            t.recordBytes = Be32(h + 4);
            t.capacity = Be16(h + 16);
            t.count = Be16(h + 18);
            const uint32_t fieldCount = h[24];
            size_t at = t.start + kTableHeader;
            if (at + size_t(fieldCount) * 16 > offset + size_)
                return false;
            for (uint32_t k = 0; k < fieldCount; k++, at += 16)
                t.fields.push_back({ FieldKind(Be32(&d[at])), Be32(&d[at + 4]), Be32(&d[at + 12]), Tag(&d[at + 8]) });
            t.dataStart = at;
            tables_.push_back(std::move(t));
        }
        for (Table& t : tables_)
            t.db_ = this;
        return true;
    }

    Table* Database::Find(const std::string& name)
    {
        for (Table& t : tables_)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    size_t Database::TableEnd(const Table& table) const
    {
        size_t end = offset_ + size_;
        for (const Table& t : tables_)
            if (t.start > table.start && t.start < end)
                end = t.start;
        return end;
    }

    void Database::FixTableCrcs()
    {
        for (const Table& t : tables_)
        {
            const size_t end = TableEnd(t), from = t.start + kTableHeader;
            if (end >= from + 4)
                PutBe32(&(*data_)[end - 4], TableCrc(&(*data_)[from], end - 4 - from));
        }
    }

    bool Database::TableCrcsValid() const
    {
        for (const Table& t : tables_)
        {
            const size_t end = TableEnd(t), from = t.start + kTableHeader;
            if (end >= from + 4 && Be32(&(*data_)[end - 4]) != TableCrc(&(*data_)[from], end - 4 - from))
                return false;
        }
        return true;
    }

    // Header (big-endian): "MC02", u32 file size, u32 section 1 size, u32 section 2 size, u32 crc of section 1,
    // u32 crc of section 2, u32 crc of the 24 bytes before it; then section 1 (size and timestamp) and section 2
    // (the database).
    bool SaveFile::Load(const std::filesystem::path& path, std::wstring& error)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            error = L"Could not open " + path.wstring();
            return false;
        }
        data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        static const uint8_t kMagic[] = { 'D', 'B', 0, 8 };
        const auto found = std::search(data.begin(), data.end(), std::begin(kMagic), std::end(kMagic));
        if (data.size() < 28 || std::memcmp(data.data(), "MC02", 4) != 0 || found == data.end() ||
            !db.Open(&data, size_t(found - data.begin())))
        {
            error = path.filename().wstring() + L" is not a Head Coach 09 save.";
            return false;
        }
        return true;
    }

    bool SaveFile::ChecksumsValid() const
    {
        const size_t size1 = Be32(&data[8]), size2 = Be32(&data[12]);
        if (28 + size1 + size2 > data.size())
            return false;
        return EaCrc(&data[28], size1) == Be32(&data[16]) && EaCrc(&data[28 + size1], size2) == Be32(&data[20]) &&
               EaCrc(data.data(), 24) == Be32(&data[24]) && db.TableCrcsValid();
    }

    bool SaveFile::Save(const std::filesystem::path& path, std::wstring& error)
    {
        db.FixTableCrcs();
        const size_t size1 = Be32(&data[8]), size2 = Be32(&data[12]);
        if (28 + size1 + size2 > data.size())
        {
            error = L"The save's sections do not fit its size.";
            return false;
        }
        PutBe32(&data[16], EaCrc(&data[28], size1));
        PutBe32(&data[20], EaCrc(&data[28 + size1], size2));
        PutBe32(&data[24], EaCrc(data.data(), 24));
        // Written beside the file and moved over it, so a failed write leaves the old save.
        std::filesystem::path temporary = path;
        temporary += L".tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
            if (!file)
            {
                error = L"Could not write " + temporary.wstring();
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temporary, path, ec);
        if (ec)
        {
            error = L"Could not replace " + path.wstring();
            return false;
        }
        return true;
    }
}
