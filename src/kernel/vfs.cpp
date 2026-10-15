#include "kernel/vfs.h"

#include "kernel/guest.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <unordered_map>

namespace hc09
{
    namespace
    {
        constexpr uint32_t kSectorSize = 0x800;
        constexpr uint32_t kAttributeReadOnly = 0x01;
        constexpr uint32_t kAttributeDirectory = 0x10;
        constexpr uint32_t kAttributeNormal = 0x80;
        // Disc files carry no timestamps; report the game's release year (2008-08-12).
        constexpr uint64_t kDiscTime = 0x01C8FC5B6F4A0000ull;

        std::string Lower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return text;
        }

        std::vector<std::string> SplitPath(const std::string& path)
        {
            std::vector<std::string> parts;
            std::string current;
            for (char c : path)
            {
                if (c == '\\')
                {
                    if (!current.empty() && current != ".")
                        parts.push_back(current);
                    current.clear();
                }
                else
                {
                    current.push_back(c);
                }
            }
            if (!current.empty() && current != ".")
                parts.push_back(current);
            return parts;
        }

        uint64_t FileTimeValue(const FILETIME& ft)
        {
            return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        }

        // ---- Disc image (XDVDFS) -------------------------------------------------------------

        struct DiscNode
        {
            std::string name;
            bool directory = false;
            uint32_t sector = 0;
            uint64_t size = 0;
            std::map<std::string, DiscNode> children;  // keyed by lowercase name
        };

        class DiscImage
        {
        public:
            bool Open(const std::filesystem::path& path)
            {
                handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
                if (handle_ == INVALID_HANDLE_VALUE)
                    return false;

                // The game partition starts at 0 for extracted images; check the XGD offsets too.
                for (uint64_t partition : { 0ull, 0x18300000ull, 0xFD90000ull, 0x2080000ull })
                {
                    char magic[20];
                    if (ReadAt(partition + 32 * kSectorSize, magic, sizeof(magic)) == sizeof(magic) &&
                        std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", 20) == 0)
                    {
                        partition_ = partition;
                        uint32_t root[2];
                        ReadAt(partition + 32 * kSectorSize + 20, root, sizeof(root));
                        root_.directory = true;
                        LoadDirectory(root_, root[0], root[1]);
                        return true;
                    }
                }
                return false;
            }

            uint32_t ReadAt(uint64_t offset, void* buffer, uint32_t length) const
            {
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD read = 0;
                if (!ReadFile(handle_, buffer, length, &read, &overlapped))
                    return 0;
                return read;
            }

            uint64_t SectorOffset(uint32_t sector) const { return partition_ + uint64_t(sector) * kSectorSize; }

            const DiscNode* Find(const std::string& relativePath) const
            {
                const DiscNode* node = &root_;
                for (const auto& part : SplitPath(relativePath))
                {
                    auto it = node->children.find(Lower(part));
                    if (it == node->children.end())
                        return nullptr;
                    node = &it->second;
                }
                return node;
            }

        private:
            void LoadDirectory(DiscNode& directory, uint32_t sector, uint32_t size)
            {
                if (size == 0)
                    return;
                std::vector<uint8_t> table(size);
                ReadAt(SectorOffset(sector), table.data(), size);

                std::vector<uint32_t> pending{ 0 };
                std::vector<bool> seen(size / 4 + 1);
                while (!pending.empty())
                {
                    const uint32_t offset = pending.back();
                    pending.pop_back();
                    if (offset + 14 > size || seen[offset / 4])
                        continue;
                    seen[offset / 4] = true;

                    const uint8_t* entry = table.data() + offset;
                    const uint16_t left = entry[0] | (entry[1] << 8);
                    const uint16_t right = entry[2] | (entry[3] << 8);
                    if (left == 0xFFFF)
                        continue;  // padding
                    uint32_t start, length;
                    std::memcpy(&start, entry + 4, 4);
                    std::memcpy(&length, entry + 8, 4);
                    const uint8_t attributes = entry[12];
                    const uint8_t nameLength = entry[13];

                    DiscNode node;
                    node.name.assign(reinterpret_cast<const char*>(entry + 14), nameLength);
                    node.directory = (attributes & kAttributeDirectory) != 0;
                    node.sector = start;
                    node.size = length;
                    if (node.directory)
                        LoadDirectory(node, start, length);
                    directory.children.emplace(Lower(node.name), std::move(node));

                    if (left)
                        pending.push_back(left * 4u);
                    if (right)
                        pending.push_back(right * 4u);
                }
            }

            HANDLE handle_ = INVALID_HANDLE_VALUE;
            uint64_t partition_ = 0;
            DiscNode root_;
        };

        DiscImage g_disc;

        class DiscFile : public File
        {
        public:
            explicit DiscFile(const DiscNode* node) : node_(node) {}

            FileInfo Info() const override
            {
                FileInfo info;
                info.directory = node_->directory;
                info.size = node_->directory ? 0 : node_->size;
                info.creationTime = info.writeTime = kDiscTime;
                info.attributes = kAttributeReadOnly | (node_->directory ? kAttributeDirectory : 0);
                return info;
            }

            uint32_t Read(uint64_t offset, void* buffer, uint32_t length) override
            {
                if (node_->directory || offset >= node_->size)
                    return 0;
                length = uint32_t(std::min<uint64_t>(length, node_->size - offset));
                return g_disc.ReadAt(g_disc.SectorOffset(node_->sector) + offset, buffer, length);
            }

            std::vector<DirectoryEntry> List() const override
            {
                std::vector<DirectoryEntry> entries;
                for (const auto& [key, child] : node_->children)
                    entries.push_back({ child.name, DiscFile(&child).Info() });
                return entries;
            }

        private:
            const DiscNode* node_;
        };

        // ---- Host directories ----------------------------------------------------------------

        FileInfo HostInfo(const WIN32_FILE_ATTRIBUTE_DATA& data)
        {
            FileInfo info;
            info.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            info.size = info.directory ? 0 : (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            info.creationTime = FileTimeValue(data.ftCreationTime);
            info.writeTime = FileTimeValue(data.ftLastWriteTime);
            info.attributes = info.directory ? kAttributeDirectory : kAttributeNormal;
            return info;
        }

        class HostFile : public File
        {
        public:
            HostFile(HANDLE handle, std::filesystem::path path, bool writable)
                : handle_(handle), path_(std::move(path)), writable_(writable) {}
            ~HostFile() override
            {
                CloseHandle(handle_);
                if (deleteOnClose_)
                    DeleteFileW(path_.c_str());
            }

            uint32_t SetDeleteOnClose(bool remove) override
            {
                deleteOnClose_ = remove;
                return status::kSuccess;
            }

            FileInfo Info() const override
            {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &data);
                return HostInfo(data);
            }

            uint32_t Read(uint64_t offset, void* buffer, uint32_t length) override
            {
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD read = 0;
                return ReadFile(handle_, buffer, length, &read, &overlapped) ? read : 0;
            }

            uint32_t Write(uint64_t offset, const void* buffer, uint32_t length) override
            {
                OVERLAPPED overlapped{};
                overlapped.Offset = DWORD(offset);
                overlapped.OffsetHigh = DWORD(offset >> 32);
                DWORD written = 0;
                return WriteFile(handle_, buffer, length, &written, &overlapped) ? written : 0;
            }

            bool SetSize(uint64_t size) override
            {
                FILE_END_OF_FILE_INFO info{};
                info.EndOfFile.QuadPart = LONGLONG(size);
                return SetFileInformationByHandle(handle_, FileEndOfFileInfo, &info, sizeof(info)) != 0;
            }

            bool Writable() const override { return writable_; }

        private:
            HANDLE handle_;
            std::filesystem::path path_;
            bool writable_;
            bool deleteOnClose_ = false;
        };

        class HostDirectory : public File
        {
        public:
            // The root of a host device (e.g. cache:\.) is never deleted.
            HostDirectory(std::filesystem::path path, bool deviceRoot)
                : path_(std::move(path).lexically_normal()), deviceRoot_(deviceRoot) {}
            ~HostDirectory() override
            {
                if (deleteOnClose_)
                    RemoveDirectoryW(path_.c_str());
            }

            uint32_t SetDeleteOnClose(bool remove) override
            {
                if (remove && deviceRoot_)
                    return status::kCannotDelete;
                std::error_code ec;
                if (remove && !std::filesystem::is_empty(path_, ec))
                    return status::kDirectoryNotEmpty;
                deleteOnClose_ = remove;
                return status::kSuccess;
            }

            FileInfo Info() const override
            {
                WIN32_FILE_ATTRIBUTE_DATA data{};
                GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &data);
                return HostInfo(data);
            }

            uint32_t Read(uint64_t, void*, uint32_t) override { return 0; }

            std::vector<DirectoryEntry> List() const override
            {
                std::vector<DirectoryEntry> entries;
                std::error_code ec;
                for (const auto& item : std::filesystem::directory_iterator(path_, ec))
                {
                    WIN32_FILE_ATTRIBUTE_DATA data{};
                    GetFileAttributesExW(item.path().c_str(), GetFileExInfoStandard, &data);
                    entries.push_back({ item.path().filename().string(), HostInfo(data) });
                }
                return entries;
            }

        private:
            std::filesystem::path path_;
            bool deviceRoot_;
            bool deleteOnClose_ = false;
        };

        // ---- Devices and links ---------------------------------------------------------------

        struct Device
        {
            std::string name;                 // canonical spelling, e.g. \Device\Cdrom0
            bool disc = false;
            std::filesystem::path hostRoot;   // for host-backed devices
        };

        std::mutex g_vfsMutex;
        std::map<std::string, Device> g_devices;                 // lowercase device -> device
        std::unordered_map<std::string, std::string> g_links;    // lowercase "game:" -> target
        std::filesystem::path g_userData;

        // Splits a canonical path into its device and the path relative to it.
        const Device* FindDevice(const std::string& canonical, std::string& relative)
        {
            const std::string lower = Lower(canonical);
            const Device* best = nullptr;
            size_t bestLength = 0;
            for (const auto& [key, device] : g_devices)
            {
                if (lower.compare(0, key.size(), key) == 0 && (lower.size() == key.size() || lower[key.size()] == '\\') &&
                    key.size() > bestLength)
                {
                    best = &device;
                    bestLength = key.size();
                }
            }
            if (best)
                relative = canonical.substr(bestLength);
            return best;
        }

        std::filesystem::path HostPath(const Device& device, const std::string& relative)
        {
            std::filesystem::path path = device.hostRoot;
            for (const auto& part : SplitPath(relative))
            {
                if (part == "..")
                    continue;  // never escape the device root
                path /= part;
            }
            return path;
        }

        OpenResult OpenOnHost(const Device& device, const std::string& canonical, const std::string& relative,
            OpenDisposition disposition, bool directory, bool write)
        {
            // FATX has no "." or ".." entries and rejects them as names. The title's replay cleanup
            // ends by deleting "cache:\Replays\.", which must fail and leave the directory in place.
            for (size_t start = 0; start <= relative.size();)
            {
                size_t end = relative.find('\\', start);
                if (end == std::string::npos)
                    end = relative.size();
                const std::string_view part(relative.data() + start, end - start);
                if (part == "." || part == "..")
                    return { status::kObjectNameInvalid };
                start = end + 1;
            }
            const auto path = HostPath(device, relative);
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            const bool isDirectory = exists && std::filesystem::is_directory(path, ec);

            if (exists && disposition == OpenDisposition::Create)
                return { status::kObjectNameCollision };
            if (!exists && (disposition == OpenDisposition::Open || disposition == OpenDisposition::Overwrite))
            {
                const bool parentExists = std::filesystem::exists(path.parent_path(), ec);
                return { parentExists ? status::kObjectNameNotFound : status::kObjectPathNotFound };
            }

            if (directory || isDirectory)
            {
                if (!exists && !std::filesystem::create_directories(path, ec))
                    return { status::kObjectPathNotFound };
                return { status::kSuccess, exists ? 1u : 2u,
                    std::make_shared<FileObject>(canonical, std::make_unique<HostDirectory>(path,
                        path.lexically_normal() == device.hostRoot.lexically_normal())) };
            }

            DWORD creation = OPEN_EXISTING;
            uint32_t information = 1;  // FILE_OPENED
            switch (disposition)
            {
            case OpenDisposition::Open: creation = OPEN_EXISTING; break;
            case OpenDisposition::Create: creation = CREATE_NEW; information = 2; break;
            case OpenDisposition::OpenOrCreate: creation = OPEN_ALWAYS; information = exists ? 1 : 2; break;
            case OpenDisposition::Overwrite: creation = TRUNCATE_EXISTING; information = 3; break;
            case OpenDisposition::Supersede: creation = CREATE_ALWAYS; information = exists ? 3 : 2; break;
            }
            const bool writable = write || disposition != OpenDisposition::Open;
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | (writable ? GENERIC_WRITE : 0),
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return { GetLastError() == ERROR_PATH_NOT_FOUND ? status::kObjectPathNotFound : status::kAccessDenied };
            return { status::kSuccess, information,
                std::make_shared<FileObject>(canonical, std::make_unique<HostFile>(handle, path, writable)) };
        }
    }

    bool InitializeVfs(const std::filesystem::path& isoPath, const std::filesystem::path& userData)
    {
        if (!g_disc.Open(isoPath))
        {
            std::fprintf(stderr, "[vfs] cannot read disc image %s\n", isoPath.string().c_str());
            return false;
        }
        g_userData = userData;
        std::filesystem::create_directories(userData / "hdd");
        std::filesystem::create_directories(userData / "cache");

        g_devices["\\device\\cdrom0"] = { "\\Device\\Cdrom0", true, {} };
        MountHostDirectory("\\Device\\Harddisk0\\Partition1", userData / "hdd");
        MountHostDirectory("\\Device\\Harddisk0\\Cache0", userData / "cache");
        MountHostDirectory("\\Device\\Harddisk0\\Cache1", userData / "cache");
        AddSymbolicLink("game:", "\\Device\\Cdrom0");
        AddSymbolicLink("d:", "\\Device\\Cdrom0");
        AddSymbolicLink("cache:", "\\Device\\Harddisk0\\Cache0");
        return true;
    }

    std::filesystem::path UserDataPath()
    {
        return g_userData;
    }

    void MountHostDirectory(const std::string& device, const std::filesystem::path& directory)
    {
        std::filesystem::create_directories(directory);
        std::lock_guard lock(g_vfsMutex);
        g_devices[Lower(device)] = { device, false, directory };
    }

    void Unmount(const std::string& device)
    {
        std::lock_guard lock(g_vfsMutex);
        g_devices.erase(Lower(device));
    }

    void AddSymbolicLink(const std::string& link, const std::string& target)
    {
        std::string name = link;
        if (name.rfind("\\??\\", 0) == 0)
            name = name.substr(4);
        std::lock_guard lock(g_vfsMutex);
        g_links[Lower(name)] = target;
    }

    void RemoveSymbolicLink(const std::string& link)
    {
        std::string name = link;
        if (name.rfind("\\??\\", 0) == 0)
            name = name.substr(4);
        std::lock_guard lock(g_vfsMutex);
        g_links.erase(Lower(name));
    }

    std::string CanonicalizePath(const std::string& guestPath)
    {
        std::string path = guestPath;
        std::replace(path.begin(), path.end(), '/', '\\');
        if (path.rfind("\\??\\", 0) == 0)
            path = path.substr(4);

        // name:\rest -> link target + \rest
        const size_t colon = path.find(':');
        if (colon != std::string::npos && path.find('\\') > colon)
        {
            std::lock_guard lock(g_vfsMutex);
            auto it = g_links.find(Lower(path.substr(0, colon + 1)));
            if (it != g_links.end())
            {
                std::string rest = path.substr(colon + 1);
                if (!rest.empty() && rest[0] != '\\')
                    rest = "\\" + rest;
                path = it->second + rest;
            }
        }
        while (path.size() > 1 && path.back() == '\\')
            path.pop_back();
        return path;
    }

    OpenResult OpenFile(const std::string& guestPath, OpenDisposition disposition, bool directory, bool write)
    {
        const std::string canonical = CanonicalizePath(guestPath);
        std::string relative;
        const Device* device;
        {
            std::lock_guard lock(g_vfsMutex);
            device = FindDevice(canonical, relative);
        }
        if (!device)
            return { status::kObjectPathNotFound };

        if (device->disc)
        {
            const DiscNode* node = g_disc.Find(relative);
            if (!node)
                return { disposition == OpenDisposition::Open ? status::kObjectNameNotFound : status::kAccessDenied };
            if (disposition == OpenDisposition::Create)
                return { status::kObjectNameCollision };
            if (write || disposition == OpenDisposition::Overwrite || disposition == OpenDisposition::Supersede)
                return { status::kAccessDenied };
            return { status::kSuccess, 1, std::make_shared<FileObject>(canonical, std::make_unique<DiscFile>(node)) };
        }
        return OpenOnHost(*device, canonical, relative, disposition, directory, write);
    }

    std::optional<FileInfo> QueryFile(const std::string& guestPath)
    {
        auto result = OpenFile(guestPath, OpenDisposition::Open, false, false);
        if (!result.object)
            return std::nullopt;
        return result.object->file->Info();
    }
}
