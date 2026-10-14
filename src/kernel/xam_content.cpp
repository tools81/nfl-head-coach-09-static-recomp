// XAM content packages (saves, downloadable content) stored as host directories:
//   <userdata>/content/<content type, hex>/<package file name>/
// XamContentCreateEx mounts a package under the root name the game picks (e.g. "save"),
// so "save:\file" resolves into the package directory.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/vfs.h"
#include "kernel/xam.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kHardDriveDeviceId = 1;
        constexpr uint32_t kContentDataSize = 0x134;  // XCONTENT_DATA
        constexpr uint32_t kFileNameLength = 42;

        // XamContentCreateEx disposition values
        constexpr uint32_t kCreateNew = 1;
        constexpr uint32_t kCreateAlways = 2;
        constexpr uint32_t kOpenExisting = 3;
        constexpr uint32_t kOpenAlways = 4;
        constexpr uint32_t kTruncateExisting = 5;

        struct ContentData
        {
            uint32_t deviceId = kHardDriveDeviceId;
            uint32_t contentType = 0;
            std::u16string displayName;
            std::string fileName;
        };

        // XCONTENT_DATA: be32 device_id; be32 content_type; be16 display_name[128];
        // char file_name[42]; padding to 0x134.
        ContentData LoadContentData(uint32_t address)
        {
            ContentData data;
            data.deviceId = Load<uint32_t>(address);
            data.contentType = Load<uint32_t>(address + 4);
            data.displayName = LoadU16String(address + 8);
            const char* name = reinterpret_cast<const char*>(GuestToHost(address + 0x108));
            data.fileName.assign(name, strnlen(name, kFileNameLength));
            return data;
        }

        void StoreContentData(uint32_t address, const ContentData& data)
        {
            std::memset(GuestToHost(address), 0, kContentDataSize);
            Store<uint32_t>(address, data.deviceId);
            Store<uint32_t>(address + 4, data.contentType);
            for (size_t i = 0; i < data.displayName.size() && i < 127; i++)
                Store<uint16_t>(address + 8 + uint32_t(i) * 2, data.displayName[i]);
            std::memcpy(GuestToHost(address + 0x108), data.fileName.data(), std::min<size_t>(data.fileName.size(), kFileNameLength));
        }

        std::filesystem::path TypeDirectory(uint32_t contentType)
        {
            char name[16];
            std::snprintf(name, sizeof(name), "%08X", contentType);
            return UserDataPath() / "content" / name;
        }

        std::filesystem::path PackageDirectory(const ContentData& data)
        {
            return TypeDirectory(data.contentType) / data.fileName;
        }

        // Display names are kept beside the package directory.
        void SaveDisplayName(const ContentData& data)
        {
            FILE* file = _wfopen((PackageDirectory(data).wstring() + L".name").c_str(), L"wb");
            if (!file)
                return;
            std::fwrite(data.displayName.data(), 2, data.displayName.size(), file);
            std::fclose(file);
        }

        std::u16string ReadDisplayName(const std::filesystem::path& package)
        {
            std::u16string name;
            FILE* file = _wfopen((package.wstring() + L".name").c_str(), L"rb");
            if (!file)
                return name;
            char16_t c;
            while (std::fread(&c, 2, 1, file) == 1)
                name.push_back(c);
            std::fclose(file);
            return name;
        }

        class ContentEnumerator : public KernelObject
        {
        public:
            ContentEnumerator() : KernelObject(ObjectType::Other) {}
            std::vector<ContentData> items;
            size_t next = 0;
            uint32_t itemsPerCall = 1;
        };

        uint32_t StackArg32(const PPCContext& ctx, int index)
        {
            return Load<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8);
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    // (user_index, device_id, content_type, content_flags, items_per_enumerate,
    //  out_buffer_size, out_handle)
    const uint32_t deviceId = Arg32(ctx, 1), contentType = Arg32(ctx, 2);
    const uint32_t itemsPerCall = std::max(Arg32(ctx, 4), 1u);
    auto enumerator = std::make_shared<ContentEnumerator>();
    enumerator->itemsPerCall = itemsPerCall;
    if (deviceId == 0 || deviceId == kHardDriveDeviceId)
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(TypeDirectory(contentType), ec))
        {
            if (!entry.is_directory())
                continue;
            ContentData data;
            data.contentType = contentType;
            data.fileName = entry.path().filename().string();
            data.displayName = ReadDisplayName(entry.path());
            enumerator->items.push_back(std::move(data));
        }
    }
    std::fprintf(stderr, "[content] enumerate type 0x%X: %zu packages\n", contentType, enumerator->items.size());
    if (Arg32(ctx, 5))
        Store<uint32_t>(Arg32(ctx, 5), itemsPerCall * kContentDataSize);
    Store<uint32_t>(Arg32(ctx, 6), CreateHandle(enumerator));
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamEnumerate)
{
    // (handle, flags, buffer, buffer_length, out_items_returned, overlapped)
    auto object = LookupHandle(Arg32(ctx, 0));
    auto* enumerator = dynamic_cast<ContentEnumerator*>(object.get());
    const uint32_t buffer = Arg32(ctx, 2), length = Arg32(ctx, 3), returnedPtr = Arg32(ctx, 4), overlapped = Arg32(ctx, 5);
    if (!enumerator)
    {
        std::fprintf(stderr, "[content] XamEnumerate on unknown handle 0x%X\n", Arg32(ctx, 0));
        return Return(ctx, CompleteRequest(overlapped, error::kInvalidHandle));
    }
    const uint32_t capacity = std::min(length / kContentDataSize, enumerator->itemsPerCall);
    uint32_t count = 0;
    while (count < capacity && enumerator->next < enumerator->items.size())
        StoreContentData(buffer + count++ * kContentDataSize, enumerator->items[enumerator->next++]);
    if (returnedPtr)
        Store<uint32_t>(returnedPtr, count);
    const uint32_t result = count ? error::kSuccess : error::kNoMoreFiles;
    if (overlapped)
    {
        CompleteOverlapped(overlapped, result, count, result);
        return Return(ctx, error::kIoPending);
    }
    Return(ctx, result);
}

PPC_FUNC(__imp__XamContentCreateEx)
{
    // (user_index, root_name, content_data, disposition, out_disposition, out_license_mask,
    //  cache_size, content_size (64-bit), overlapped)
    const std::string root = GuestCString(Arg32(ctx, 1));
    const ContentData data = LoadContentData(Arg32(ctx, 2));
    const uint32_t disposition = Arg32(ctx, 3) & 0xFF;
    const uint32_t dispositionOut = Arg32(ctx, 4), licenseOut = Arg32(ctx, 5);
    const uint32_t overlapped = StackArg32(ctx, 8);

    const auto directory = PackageDirectory(data);
    std::error_code ec;
    const bool exists = std::filesystem::exists(directory, ec);
    uint32_t result = error::kSuccess;
    bool created = false;
    switch (disposition)
    {
    case kCreateNew:
        if (exists)
            result = error::kAlreadyExists;
        else
            created = true;
        break;
    case kCreateAlways:
    case kTruncateExisting:
        if (disposition == kTruncateExisting && !exists)
        {
            result = error::kFileNotFound;
            break;
        }
        std::filesystem::remove_all(directory, ec);
        created = true;
        break;
    case kOpenExisting:
        if (!exists)
            result = error::kFileNotFound;
        break;
    case kOpenAlways:
    default:
        created = !exists;
        break;
    }

    std::fprintf(stderr, "[content] %s \"%s\" (type 0x%X) as %s: -> %u\n", created ? "create" : "open", data.fileName.c_str(),
        data.contentType, root.c_str(), result);
    if (result == error::kSuccess)
    {
        std::filesystem::create_directories(directory, ec);
        if (created)
            SaveDisplayName(data);
        const std::string device = "\\Device\\Content\\" + root;
        MountHostDirectory(device, directory);
        AddSymbolicLink(root + ":", device);
        if (dispositionOut)
            Store<uint32_t>(dispositionOut, created ? 1 : 2);
        if (licenseOut)
            Store<uint32_t>(licenseOut, 0);
    }
    Return(ctx, CompleteRequest(overlapped, result));
}

PPC_FUNC(__imp__XamContentClose)
{
    // (root_name, overlapped)
    const std::string root = GuestCString(Arg32(ctx, 0));
    RemoveSymbolicLink(root + ":");
    Unmount("\\Device\\Content\\" + root);
    Return(ctx, CompleteRequest(Arg32(ctx, 1), error::kSuccess));
}

PPC_FUNC(__imp__XamContentDelete)
{
    // (user_index, content_data, overlapped)
    const ContentData data = LoadContentData(Arg32(ctx, 1));
    std::error_code ec;
    std::filesystem::remove_all(PackageDirectory(data), ec);
    std::filesystem::remove(PackageDirectory(data).wstring() + L".name", ec);
    Return(ctx, CompleteRequest(Arg32(ctx, 2), error::kSuccess));
}

PPC_FUNC(__imp__XamContentGetCreator)
{
    // (user_index, content_data, out_is_creator, out_creator_xuid, overlapped)
    if (Arg32(ctx, 2))
        Store<uint32_t>(Arg32(ctx, 2), 1);
    if (Arg32(ctx, 3))
        Store<uint64_t>(Arg32(ctx, 3), kLocalXuid);
    Return(ctx, CompleteRequest(Arg32(ctx, 4), error::kSuccess));
}

PPC_FUNC(__imp__XamContentSetThumbnail)
{
    Return(ctx, CompleteRequest(Arg32(ctx, 4), error::kSuccess));
}

PPC_FUNC(__imp__XamContentGetDeviceState)
{
    // (device_id, overlapped)
    const uint32_t deviceId = Arg32(ctx, 0);
    const uint32_t result = (deviceId == kHardDriveDeviceId) ? error::kSuccess : error::kDeviceNotConnected;
    Return(ctx, CompleteRequest(Arg32(ctx, 1), result));
}

PPC_FUNC(__imp__XamContentGetDeviceData)
{
    // (device_id, out X_DEVICE_DATA: be32 id; be32 type; be64 total; be64 free; be16 name[28])
    const uint32_t deviceId = Arg32(ctx, 0), out = Arg32(ctx, 1);
    if (deviceId != kHardDriveDeviceId)
        return Return(ctx, error::kDeviceNotConnected);
    std::memset(GuestToHost(out), 0, 0x50);
    Store<uint32_t>(out + 0x00, kHardDriveDeviceId);
    Store<uint32_t>(out + 0x04, 1);  // hard drive
    Store<uint64_t>(out + 0x08, 20ull << 30);
    Store<uint64_t>(out + 0x10, 16ull << 30);
    const char* name = "Hard Drive";
    for (uint32_t i = 0; name[i]; i++)
        Store<uint16_t>(out + 0x18 + i * 2, uint16_t(name[i]));
    Return(ctx, error::kSuccess);
}
