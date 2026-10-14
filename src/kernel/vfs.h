#pragma once

// Virtual file system for guest paths.
//
//   \Device\Cdrom0\...               the game disc, read straight from the ISO (XDVDFS)
//   \Device\Harddisk0\Partition1\... host directory <userdata>/hdd
//   \Device\Harddisk0\Cache0|1\...   host directory <userdata>/cache
//   name:\... and \??\name:\...      resolved through the symbolic link table
//                                    (game: and d: point at the disc by default)

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "kernel/objects.h"

namespace hc09
{
    struct FileInfo
    {
        bool directory = false;
        uint64_t size = 0;
        uint64_t creationTime = 0;  // FILETIME
        uint64_t writeTime = 0;
        uint32_t attributes = 0;    // FILE_ATTRIBUTE_*
    };

    struct DirectoryEntry
    {
        std::string name;
        FileInfo info;
    };

    // An open file or directory. Reads/writes are positional; the handle's current
    // position is kept in FileObject.
    class File
    {
    public:
        virtual ~File() = default;
        virtual FileInfo Info() const = 0;
        virtual uint32_t Read(uint64_t offset, void* buffer, uint32_t length) = 0;
        virtual uint32_t Write(uint64_t offset, const void* buffer, uint32_t length) { return 0; }
        virtual bool SetSize(uint64_t /*size*/) { return false; }
        virtual bool Writable() const { return false; }
        virtual std::vector<DirectoryEntry> List() const { return {}; }
    };

    class FileObject : public KernelObject
    {
    public:
        FileObject(std::string path, std::unique_ptr<File> file)
            : KernelObject(ObjectType::File), path(std::move(path)), file(std::move(file)) {}

        std::string path;  // canonical guest path, e.g. \Device\Cdrom0\qkl_boot.ast
        std::unique_ptr<File> file;
        std::mutex mutex;  // guards position and directory enumeration state
        uint64_t position = 0;
        std::vector<DirectoryEntry> listing;
        size_t listingIndex = 0;
        bool listingLoaded = false;
    };

    enum class OpenDisposition
    {
        Open,          // FILE_OPEN: must exist
        Create,        // FILE_CREATE: must not exist
        OpenOrCreate,  // FILE_OPEN_IF
        Overwrite,     // FILE_OVERWRITE: must exist, truncate
        Supersede,     // FILE_SUPERSEDE / FILE_OVERWRITE_IF: create or truncate
    };

    struct OpenResult
    {
        uint32_t status = 0;
        uint32_t information = 0;  // FILE_OPENED / FILE_CREATED / FILE_OVERWRITTEN ...
        std::shared_ptr<FileObject> object;
    };

    // Must be called once at startup. `userData` is created if missing.
    bool InitializeVfs(const std::filesystem::path& isoPath, const std::filesystem::path& userData);

    // Resolves symbolic links and drive prefixes to a canonical device path.
    std::string CanonicalizePath(const std::string& guestPath);

    OpenResult OpenFile(const std::string& guestPath, OpenDisposition disposition, bool directory, bool write);
    std::optional<FileInfo> QueryFile(const std::string& guestPath);

    void AddSymbolicLink(const std::string& link, const std::string& target);
    void RemoveSymbolicLink(const std::string& link);

    // Maps an extra device (e.g. a content package) onto a host directory.
    void MountHostDirectory(const std::string& device, const std::filesystem::path& directory);
    void Unmount(const std::string& device);
    std::filesystem::path UserDataPath();
}
