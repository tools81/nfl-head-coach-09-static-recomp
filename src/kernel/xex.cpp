// XEX header and module functions: RtlImageXexHeaderField, XexCheckExecutablePrivilege,
// XexGetModuleHandle, XexGetProcedureAddress.

#include "kernel/xex.h"

#include <cstdio>
#include <fstream>
#include <vector>

#include "kernel/guest.h"
#include "kernel/heap.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kSystemFlagsKey = 0x00030000;
        constexpr uint32_t kEntryPointKey = 0x00010100;

        // X_LDR_DATA_TABLE_ENTRY (offsets from Xenia)
        constexpr uint32_t kLdrSize = 0x80;
        constexpr uint32_t kLdrDllBase = 0x18;
        constexpr uint32_t kLdrEntryPoint = 0x1C;
        constexpr uint32_t kLdrSizeOfImage = 0x20;
        constexpr uint32_t kLdrFullDllName = 0x24;
        constexpr uint32_t kLdrBaseDllName = 0x2C;
        constexpr uint32_t kLdrFlags = 0x34;
        constexpr uint32_t kLdrLoadCount = 0x40;
        constexpr uint32_t kLdrXexHeaderBase = 0x58;

        uint32_t g_header = 0;
        uint32_t g_module = 0;

        uint32_t StoreUnicodeString(uint32_t stringStruct, const char* text)
        {
            const uint32_t length = uint32_t(std::strlen(text));
            const uint32_t buffer = PoolHeap().Allocate((length + 1) * 2);
            for (uint32_t i = 0; i < length; i++)
                Store<uint16_t>(buffer + i * 2, uint8_t(text[i]));
            Store<uint16_t>(stringStruct, uint16_t(length * 2));
            Store<uint16_t>(stringStruct + 2, uint16_t((length + 1) * 2));
            Store<uint32_t>(stringStruct + 4, buffer);
            return buffer;
        }
    }

    bool LoadXexHeader(const char* path)
    {
        std::ifstream file(path, std::ios::binary);
        char fixed[0x18];
        if (!file.read(fixed, sizeof(fixed)) || std::memcmp(fixed, "XEX2", 4) != 0)
            return false;
        const uint32_t headerSize = __builtin_bswap32(*reinterpret_cast<uint32_t*>(fixed + 8));
        std::vector<char> header(headerSize);
        file.seekg(0);
        if (!file.read(header.data(), headerSize))
            return false;

        g_header = PoolHeap().Allocate(headerSize, 0x1000);
        std::memcpy(GuestToHost(g_header), header.data(), headerSize);

        g_module = PoolHeap().Allocate(kLdrSize);
        Store<uint32_t>(g_module + kLdrDllBase, uint32_t(PPC_IMAGE_BASE));
        Store<uint32_t>(g_module + kLdrEntryPoint, XexHeaderField(g_header, kEntryPointKey));
        Store<uint32_t>(g_module + kLdrSizeOfImage, uint32_t(PPC_IMAGE_SIZE));
        StoreUnicodeString(g_module + kLdrFullDllName, "\\Device\\Cdrom0\\default.xex");
        StoreUnicodeString(g_module + kLdrBaseDllName, "default.xex");
        Store<uint32_t>(g_module + kLdrFlags, 0);
        Store<uint32_t>(g_module + kLdrLoadCount, 1);
        Store<uint32_t>(g_module + kLdrXexHeaderBase, g_header);
        return true;
    }

    uint32_t XexHeaderAddress() { return g_header; }
    uint32_t XexModuleHandle() { return g_module; }

    uint32_t XexHeaderField(uint32_t headerAddress, uint32_t key)
    {
        const uint32_t count = Load<uint32_t>(headerAddress + 0x14);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t entry = headerAddress + 0x18 + i * 8;
            if (Load<uint32_t>(entry) != key)
                continue;
            switch (key & 0xFF)
            {
            case 0x00: return Load<uint32_t>(entry + 4);   // value stored inline
            case 0x01: return entry + 4;                    // address of the inline value
            default: return headerAddress + Load<uint32_t>(entry + 4);  // offset to data
            }
        }
        return 0;
    }
}

using namespace hc09;

PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    Return(ctx, XexHeaderField(Arg32(ctx, 0), Arg32(ctx, 1)));
}

PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    const uint32_t privilege = Arg32(ctx, 0);
    const uint32_t flags = XexHeaderField(g_header, kSystemFlagsKey);
    Return(ctx, privilege < 32 ? (flags >> privilege) & 1 : 0);
}

PPC_FUNC(__imp__XexGetModuleHandle)
{
    // (name, out_handle). Only the executable itself (null name or default.xex) exists;
    // xam.xex and xboxkrnl.exe are host code.
    const char* name = GuestCString(Arg32(ctx, 0));
    const uint32_t out = Arg32(ctx, 1);
    if (name == nullptr || _stricmp(name, "default.xex") == 0)
    {
        Store<uint32_t>(out, g_module);
        return Return(ctx, status::kSuccess);
    }
    std::fprintf(stderr, "[xex] XexGetModuleHandle(\"%s\") -> not found\n", name);
    Store<uint32_t>(out, 0);
    Return(ctx, 0xC0000135);  // STATUS_DLL_NOT_FOUND
}

PPC_FUNC(__imp__XexGetProcedureAddress)
{
    // (module, ordinal, out_address). Dynamic lookups of host exports are not supported yet.
    std::fprintf(stderr, "[xex] XexGetProcedureAddress(0x%08X, %u) unsupported\n", Arg32(ctx, 0), Arg32(ctx, 1));
    Store<uint32_t>(Arg32(ctx, 2), 0);
    Return(ctx, 0xC0000139);  // STATUS_ENTRYPOINT_NOT_FOUND
}
