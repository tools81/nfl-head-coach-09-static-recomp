"""Reader/writer for EA TDB table databases as stored in NFL Head Coach 09 (Xbox 360) saves.

A save (e.g. ROSTER-DEFAULT) is an "MC02" container whose payload is a big-endian TDB:

    header      "DB" 00 08, u32 ?, u32 database size, u32 0, u32 table count, u32 crc
    directory   table count x (u32 name, u32 offset from the end of the directory), u32 crc
    tables      header, field definitions, records

Table names and field names are four characters stored reversed ("YALP" is PLAY).
Records are fixed length and bit-packed; each field names its bit offset and width.
"""

import struct
import sys

FIELD_TYPES = {0: "string", 1: "binary", 2: "sint", 3: "uint", 4: "float"}


def tag(raw):
    return raw[::-1].decode("latin-1")


class Field:
    def __init__(self, kind, offset, name, bits):
        self.kind, self.offset, self.name, self.bits = kind, offset, name, bits

    def __repr__(self):
        return f"{self.name}:{FIELD_TYPES.get(self.kind, self.kind)}@{self.offset}/{self.bits}"


class Table:
    HEADER = struct.Struct(">IIIIHHIBBHII")

    def __init__(self, db, name, start):
        self.db, self.name, self.start = db, name, start
        (self.unknown1, self.record_bytes, self.record_bits, self.unknown2, self.capacity, self.count,
         self.unknown3, field_count, self.index_count, self.unknown4, self.unknown5,
         self.header_crc) = self.HEADER.unpack_from(db.data, start)
        at = start + self.HEADER.size
        self.fields = []
        for _ in range(field_count):
            kind, offset, name, bits = struct.unpack_from(">II4sI", db.data, at)
            self.fields.append(Field(kind, offset, tag(name), bits))
            at += 16
        self.fields_end = at
        self.data_start = at

    @property
    def data_end(self):  # capacity-sized; tables with no records store none
        return self.data_start + self.capacity * self.record_bytes

    def field(self, name):
        for f in self.fields:
            if f.name == name:
                return f
        raise KeyError(f"{self.name}.{name}")

    def _record(self, index):
        return self.data_start + index * self.record_bytes

    def get(self, index, name):
        f = self.field(name)
        base = self._record(index)
        if f.kind == 0:
            start = base + f.offset // 8
            raw = bytes(self.db.data[start:start + f.bits // 8])
            return raw.split(b"\0", 1)[0].decode("latin-1")
        value = self._bits(base, f.offset, f.bits)
        if f.kind == 2 and value & (1 << (f.bits - 1)):
            value -= 1 << f.bits
        if f.kind == 4:
            value = struct.unpack(">f", struct.pack(">I", value))[0]
        return value

    def set(self, index, name, value):
        f = self.field(name)
        base = self._record(index)
        if f.kind == 0:
            raw = value.encode("latin-1")[:f.bits // 8 - 1]
            start = base + f.offset // 8
            self.db.data[start:start + f.bits // 8] = raw.ljust(f.bits // 8, b"\0")
            return
        if f.kind == 4:
            value = struct.unpack(">I", struct.pack(">f", value))[0]
        value &= (1 << f.bits) - 1
        self._set_bits(base, f.offset, f.bits, value)

    def row(self, index):
        return {f.name: self.get(index, f.name) for f in self.fields}

    def _bits(self, base, offset, bits):
        value = 0
        for i in range(bits):
            bit = offset + i
            byte = self.db.data[base + bit // 8]
            value = (value << 1) | ((byte >> (7 - bit % 8)) & 1)
        return value

    def _set_bits(self, base, offset, bits, value):
        for i in range(bits):
            bit = offset + i
            mask = 1 << (7 - bit % 8)
            at = base + bit // 8
            if (value >> (bits - 1 - i)) & 1:
                self.db.data[at] |= mask
            else:
                self.db.data[at] &= ~mask & 0xFF


class Database:
    def __init__(self, data, offset):
        self.data, self.offset = data, offset
        sig, self.version, self.unknown1, self.size, self.zero, count, self.header_crc = \
            struct.unpack_from(">2sHIIIII", data, offset)
        if sig != b"DB":
            raise ValueError("not a TDB")
        directory = offset + 24
        self.directory_end = directory + count * 8 + 4
        self.tables = {}
        for i in range(count):
            name, table_offset = struct.unpack_from(">4sI", data, directory + i * 8)
            self.tables[tag(name)] = Table(self, tag(name), self.directory_end + table_offset)

    def table_extents(self):
        """(table, end) in file order; each table runs up to the next one (or the database end),
        and its last four bytes are a CRC of everything after its header."""
        ordered = sorted(self.tables.values(), key=lambda t: t.start)
        ends = [t.start for t in ordered[1:]] + [self.offset + self.size]
        return list(zip(ordered, ends))

    def fix_table_crcs(self):
        for table, end in self.table_extents():
            struct.pack_into(">I", self.data, end - 4, table_crc(self.data[table.start + Table.HEADER.size:end - 4]))

    def table_crcs_valid(self):
        return all(struct.unpack_from(">I", self.data, end - 4)[0] ==
                   table_crc(self.data[table.start + Table.HEADER.size:end - 4])
                   for table, end in self.table_extents())


def _crc_table():
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        table.append(c)
    return table


_CRC_TABLE = _crc_table()


def table_crc(data):
    """The CRC that ends each TDB table: MSB-first CRC-32, initial value 0xFFFFFFFF, no final xor."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc = _CRC_TABLE[(crc >> 24) ^ byte] ^ ((crc << 8) & 0xFFFFFFFF)
    return crc


def ea_crc(data):
    """The checksum the game uses for MC02 saves (sub_82F0D220): an MSB-first CRC-32 that
    starts from the complement of the first four bytes and shifts each later byte in."""
    if len(data) < 4:
        return 0
    crc = ~struct.unpack_from(">I", data)[0] & 0xFFFFFFFF
    for byte in data[4:]:
        crc = _CRC_TABLE[crc >> 24] ^ (((crc << 8) & 0xFFFFFFFF) | byte)
    return ~crc & 0xFFFFFFFF


class SaveFile:
    """An MC02 save holding one TDB.

    Header (big-endian): "MC02", u32 file size, u32 section 1 size, u32 section 2 size,
    u32 crc of section 1, u32 crc of section 2, u32 crc of the 24 bytes before it; then
    section 1 (size and timestamp) and section 2 (the TDB).
    """

    HEADER = struct.Struct(">4sIIIIII")

    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, "rb").read())
        if self.data[:4] != b"MC02":
            raise ValueError(f"{path}: not an MC02 save")
        self.db = Database(self.data, self.data.find(b"DB\x00\x08"))

    def sections(self):
        _, _, size1, size2, _, _, _ = self.HEADER.unpack_from(self.data)
        first = self.HEADER.size
        return (first, first + size1), (first + size1, first + size1 + size2)

    def checksums_valid(self):
        _, _, _, _, crc1, crc2, crc_header = self.HEADER.unpack_from(self.data)
        (a, b), (c, d) = self.sections()
        return (ea_crc(self.data[a:b]) == crc1 and ea_crc(self.data[c:d]) == crc2
                and ea_crc(self.data[:24]) == crc_header and self.db.table_crcs_valid())

    def fix_checksums(self):
        """Table CRCs first (they are inside section 2), then the container's."""
        self.db.fix_table_crcs()
        (a, b), (c, d) = self.sections()
        struct.pack_into(">II", self.data, 16, ea_crc(self.data[a:b]), ea_crc(self.data[c:d]))
        struct.pack_into(">I", self.data, 24, ea_crc(self.data[:24]))

    def save(self, path):
        open(path, "wb").write(self.data)


if __name__ == "__main__":
    save = SaveFile(sys.argv[1])
    db = save.db
    print(f"TDB at 0x{db.offset:X}, size 0x{db.size:X}, {len(db.tables)} tables")
    for name, t in db.tables.items():
        print(f"{name}: {t.count}/{t.capacity} records x {t.record_bytes} bytes ({t.record_bits} bits),"
              f" data 0x{t.data_start:X}-0x{t.data_end:X}")
        print("   ", " ".join(map(repr, t.fields)))
