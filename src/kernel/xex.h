#pragma once

#include <cstdint>

namespace hc09
{
    // Copies the XEX header (everything before the PE data) into guest memory and builds
    // the executable's LDR_DATA_TABLE_ENTRY. Returns false if the file is not a XEX2.
    bool LoadXexHeader(const char* path);

    uint32_t XexHeaderAddress();      // guest address of the loaded header
    uint32_t XexModuleHandle();       // guest LDR_DATA_TABLE_ENTRY for default.xex

    // Value or address of an optional header field, as RtlImageXexHeaderField returns it.
    uint32_t XexHeaderField(uint32_t headerAddress, uint32_t key);
}
