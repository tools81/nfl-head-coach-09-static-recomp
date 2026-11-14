#pragma once

// EA TDB table databases as NFL Head Coach 09 stores them (a C++ port of tools/roster_import/tdb.py).
//
// A database is big-endian: "DB" 00 08, u32 ?, u32 size, u32 0, u32 table count, u32 crc; a directory of
// (reversed four-character name, offset from the directory's end) pairs and a crc; then the tables. A table
// is a 36-byte header, 16-byte field definitions (type, bit offset, reversed name, bit count) and fixed-length
// bit-packed records, most significant bit first, ending with a CRC-32 of everything after its header.
// A roster or career save is an "MC02" container around one database.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace hc09::editor
{
    enum class FieldKind : uint32_t { String = 0, Binary = 1, Signed = 2, Unsigned = 3, Float = 4, Pooled = 13, Compressed = 14 };

    struct Field
    {
        FieldKind kind = FieldKind::Unsigned;
        uint32_t offset = 0, bits = 0;
        std::string name;
    };

    class Database;

    class Table
    {
    public:
        std::string name;
        size_t start = 0, dataStart = 0;
        uint32_t recordBytes = 0, capacity = 0, count = 0;
        std::vector<Field> fields;

        const Field* Find(const std::string& field) const;
        bool Has(const std::string& field) const { return Find(field) != nullptr; }
        // A missing field reads as 0 (or "") and ignores writes.
        int64_t Get(uint32_t record, const std::string& field) const;
        void Set(uint32_t record, const std::string& field, int64_t value);
        uint32_t Max(const std::string& field) const;  // the largest value the field holds
        // Pooled strings (the game's name tables) are read too; they cannot be written.
        std::string Text(uint32_t record, const std::string& field) const;
        void SetText(uint32_t record, const std::string& field, const std::string& text);
        size_t DataEnd() const { return dataStart + size_t(capacity) * recordBytes; }

    private:
        friend class Database;
        Database* db_ = nullptr;
        uint64_t Bits(size_t base, uint32_t offset, uint32_t bits) const;
        void SetBits(size_t base, uint32_t offset, uint32_t bits, uint64_t value);
    };

    class Database
    {
    public:
        // `data` holds the database at `offset`; it must outlive this object. False if there is none there.
        bool Open(std::vector<uint8_t>* data, size_t offset);
        Table* Find(const std::string& name);
        const std::vector<Table>& Tables() const { return tables_; }
        std::vector<Table>& Tables() { return tables_; }
        // Where the table's records and checksum end (the next table's start, or the database's end).
        size_t TableEnd(const Table& table) const;
        void FixTableCrcs();
        bool TableCrcsValid() const;
        size_t Offset() const { return offset_; }
        size_t Size() const { return size_; }
        std::vector<uint8_t>& Data() { return *data_; }
        const std::vector<uint8_t>& Data() const { return *data_; }

    private:
        std::vector<uint8_t>* data_ = nullptr;
        size_t offset_ = 0, size_ = 0;
        std::vector<Table> tables_;
    };

    // An MC02 save holding one database.
    class SaveFile
    {
    public:
        bool Load(const std::filesystem::path& path, std::wstring& error);
        bool Save(const std::filesystem::path& path, std::wstring& error);  // fixes every checksum first
        bool ChecksumsValid() const;
        Database db;
        std::vector<uint8_t> data;
    };

    uint32_t TableCrc(const uint8_t* data, size_t size);
    uint32_t EaCrc(const uint8_t* data, size_t size);
}
