// File I/O: NtCreateFile/NtOpenFile/NtReadFile/... on top of the VFS.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <windows.h>

#include "debug/stack_dump.h"
#include "kernel/guest.h"
#include "kernel/objects.h"
#include "kernel/threads.h"
#include "kernel/vfs.h"

namespace hc09
{
    namespace
    {
        // Arguments past the eighth live in the caller's parameter save area.
        uint32_t StackArg32(const PPCContext& ctx, int index)
        {
            return Load<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8);
        }

        constexpr uint64_t kUseFilePointerPosition = 0xFFFFFFFFFFFFFFFEull;
        constexpr uint32_t kFileDirectoryFile = 0x00000001;
        constexpr uint32_t kFileDeleteOnClose = 0x00001000;
        constexpr uint32_t kWriteAccessMask = 0x40000000 | 0x10000000 | 0x00000002 | 0x00000004;

        std::string PathFromAttributes(uint32_t objectAttributes)
        {
            std::string name = ObjectAttributesName(objectAttributes);
            const uint32_t root = objectAttributes ? Load<uint32_t>(objectAttributes) : 0;
            if (root)
            {
                if (auto directory = LookupHandleAs<FileObject>(root, ObjectType::File))
                    name = directory->path + (name.empty() || name[0] == '\\' ? "" : "\\") + name;
            }
            return name;
        }

        void CompleteIo(uint32_t ioStatus, uint32_t result, uint32_t information, uint32_t eventHandle)
        {
            if (ioStatus)
            {
                Store<uint32_t>(ioStatus, result);
                Store<uint32_t>(ioStatus + 4, information);
            }
            if (eventHandle)
            {
                if (auto event = LookupHandleAs<EventObject>(eventHandle, ObjectType::Event))
                {
                    std::lock_guard lock(WaitMutex());
                    event->signaled = true;
                    event->SyncToGuest();
                    NotifyWaiters();
                }
            }
        }

        OpenDisposition DispositionFromNt(uint32_t disposition)
        {
            switch (disposition)
            {
            case 0: return OpenDisposition::Supersede;      // FILE_SUPERSEDE
            case 1: return OpenDisposition::Open;           // FILE_OPEN
            case 2: return OpenDisposition::Create;         // FILE_CREATE
            case 3: return OpenDisposition::OpenOrCreate;   // FILE_OPEN_IF
            case 4: return OpenDisposition::Overwrite;      // FILE_OVERWRITE
            default: return OpenDisposition::Supersede;     // FILE_OVERWRITE_IF
            }
        }

        uint32_t Open(PPCContext& ctx, uint32_t handlePtr, uint32_t access, uint32_t objectAttributes,
            uint32_t ioStatus, OpenDisposition disposition, uint32_t options)
        {
            const std::string path = PathFromAttributes(objectAttributes);
            auto result = OpenFile(path, disposition, (options & kFileDirectoryFile) != 0, (access & kWriteAccessMask) != 0);
            std::fprintf(stderr, "[io] open \"%s\" -> 0x%08X\n", path.c_str(), result.status);
            // HC09_STACK_ON_OPEN=<text>: print the call stack of opens whose path contains it.
            static const char* stackOnOpen = std::getenv("HC09_STACK_ON_OPEN");
            if (stackOnOpen && path.find(stackOnOpen) != std::string::npos)
            {
                CONTEXT context{};
                RtlCaptureContext(&context);
                debug::PrintStackFromContext(&context);
            }
            if (result.object && (options & kFileDeleteOnClose))
                result.object->file->SetDeleteOnClose(true);
            if (result.object)
                Store<uint32_t>(handlePtr, CreateHandle(result.object));
            CompleteIo(ioStatus, result.status, result.information, 0);
            return result.status;
        }

        // FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
        void StoreNetworkOpenInfo(uint32_t address, const FileInfo& info)
        {
            Store<uint64_t>(address + 0x00, info.creationTime);
            Store<uint64_t>(address + 0x08, info.writeTime);
            Store<uint64_t>(address + 0x10, info.writeTime);
            Store<uint64_t>(address + 0x18, info.writeTime);
            Store<uint64_t>(address + 0x20, (info.size + 0xFFF) & ~0xFFFull);
            Store<uint64_t>(address + 0x28, info.size);
            Store<uint32_t>(address + 0x30, info.attributes);
            Store<uint32_t>(address + 0x34, 0);
        }

        bool WildcardMatch(const char* pattern, const char* text)
        {
            if (*pattern == 0)
                return *text == 0;
            if (*pattern == '*')
                return WildcardMatch(pattern + 1, text) || (*text && WildcardMatch(pattern, text + 1));
            if (*text && (*pattern == '?' || std::tolower(uint8_t(*pattern)) == std::tolower(uint8_t(*text))))
                return WildcardMatch(pattern + 1, text + 1);
            return false;
        }

        uint32_t ReadInto(FileObject& file, uint64_t offset, uint32_t buffer, uint32_t length, uint32_t& read)
        {
            // Into physical memory (where the GPU's copy of guest memory write-protects pages) through
            // a host buffer: the OS cannot write a protected page, and one can be protected again
            // while a read is under way, but a copy made here faults and is handled.
            if (buffer >= kPhysicalHeapBase)
            {
                thread_local std::vector<uint8_t> bounce;
                read = 0;
                while (read < length)
                {
                    const uint32_t chunk = std::min<uint32_t>(length - read, 1u << 20);
                    bounce.resize(chunk);
                    const uint32_t got = file.file->Read(offset + read, bounce.data(), chunk);
                    std::memcpy(GuestToHost(buffer + read), bounce.data(), got);
                    read += got;
                    if (got < chunk)
                        break;
                }
            }
            else
            {
                read = file.file->Read(offset, GuestToHost(buffer), length);
            }
            file.position = offset + read;
            return read == 0 && length > 0 ? status::kEndOfFile : status::kSuccess;
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__NtCreateFile)
{
    // (out_handle, desired_access, object_attributes, io_status, allocation_size, file_attributes,
    //  share_access, create_disposition, create_options)
    Return(ctx, Open(ctx, Arg32(ctx, 0), Arg32(ctx, 1), Arg32(ctx, 2), Arg32(ctx, 3),
        DispositionFromNt(Arg32(ctx, 7)), StackArg32(ctx, 8)));
}

PPC_FUNC(__imp__NtOpenFile)
{
    // (out_handle, desired_access, object_attributes, io_status, share_access, open_options)
    Return(ctx, Open(ctx, Arg32(ctx, 0), Arg32(ctx, 1), Arg32(ctx, 2), Arg32(ctx, 3),
        OpenDisposition::Open, Arg32(ctx, 5)));
}

PPC_FUNC(__imp__NtReadFile)
{
    // (handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset)
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    const uint32_t offsetPtr = Arg32(ctx, 7);
    std::lock_guard lock(file->mutex);
    uint64_t offset = offsetPtr ? Load<uint64_t>(offsetPtr) : kUseFilePointerPosition;
    if (offset == kUseFilePointerPosition)
        offset = file->position;
    uint32_t read = 0;
    const uint32_t result = ReadInto(*file, offset, Arg32(ctx, 5), Arg32(ctx, 6), read);
    static const bool traceIo = std::getenv("HC09_TRACE_IO") != nullptr;
    if (traceIo || result != status::kSuccess || read < Arg32(ctx, 6) || Arg32(ctx, 2) != 0)
        std::fprintf(stderr, "[io] read %s @0x%llX len 0x%X into 0x%08X -> 0x%08X (%u bytes, thread 0x%X)%s\n", file->path.c_str(),
            (unsigned long long)offset, Arg32(ctx, 6), Arg32(ctx, 5), result, read, CurrentThreadId(),
            Arg32(ctx, 2) ? " [APC routine ignored]" : "");
    // HC09_STACK_ON_READ=<hex offset>: print the call stack of reads at that file offset.
    static const char* stackOnRead = std::getenv("HC09_STACK_ON_READ");
    if (stackOnRead && offset == std::strtoull(stackOnRead, nullptr, 16))
    {
        CONTEXT context{};
        RtlCaptureContext(&context);
        debug::PrintStackFromContext(&context);
    }
    CompleteIo(Arg32(ctx, 4), result, read, Arg32(ctx, 1));
    Return(ctx, result);
}

PPC_FUNC(__imp__NtReadFileScatter)
{
    // (handle, event, apc_routine, apc_context, io_status, segments, length, byte_offset)
    // FILE_SEGMENT_ELEMENT[]: one be64 buffer pointer per 4 KiB page.
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    const uint32_t segments = Arg32(ctx, 5), length = Arg32(ctx, 6), offsetPtr = Arg32(ctx, 7);
    std::lock_guard lock(file->mutex);
    uint64_t offset = offsetPtr ? Load<uint64_t>(offsetPtr) : kUseFilePointerPosition;
    if (offset == kUseFilePointerPosition)
        offset = file->position;
    uint32_t total = 0;
    for (uint32_t done = 0, i = 0; done < length; done += 0x1000, i++)
    {
        const uint32_t page = uint32_t(Load<uint64_t>(segments + i * 8));
        uint32_t read = 0;
        ReadInto(*file, offset + done, page, std::min(0x1000u, length - done), read);
        total += read;
        if (read < std::min(0x1000u, length - done))
            break;
    }
    const uint32_t result = total == 0 && length > 0 ? status::kEndOfFile : status::kSuccess;
    static const bool traceIo = std::getenv("HC09_TRACE_IO") != nullptr;
    if (traceIo)
        std::fprintf(stderr, "[io] scatter read %s @0x%llX len 0x%X into 0x%08X... -> %u bytes\n", file->path.c_str(),
            (unsigned long long)offset, length, length ? uint32_t(Load<uint64_t>(segments)) : 0u, total);
    CompleteIo(Arg32(ctx, 4), result, total, Arg32(ctx, 1));
    Return(ctx, result);
}

PPC_FUNC(__imp__NtWriteFile)
{
    // (handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset)
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    if (!file->file->Writable())
        return Return(ctx, status::kAccessDenied);
    const uint32_t offsetPtr = Arg32(ctx, 7);
    std::lock_guard lock(file->mutex);
    uint64_t offset = offsetPtr ? Load<uint64_t>(offsetPtr) : kUseFilePointerPosition;
    if (offset == kUseFilePointerPosition)
        offset = file->position;
    const uint32_t written = file->file->Write(offset, GuestToHost(Arg32(ctx, 5)), Arg32(ctx, 6));
    file->position = offset + written;
    CompleteIo(Arg32(ctx, 4), status::kSuccess, written, Arg32(ctx, 1));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtFlushBuffersFile)
{
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtQueryInformationFile)
{
    // (handle, io_status, info, length, class)
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    const uint32_t info = Arg32(ctx, 2), infoClass = Arg32(ctx, 4);
    const FileInfo fi = file->file->Info();
    uint32_t written;
    switch (infoClass)
    {
    case 4:  // FileBasicInformation
        Store<uint64_t>(info + 0x00, fi.creationTime);
        Store<uint64_t>(info + 0x08, fi.writeTime);
        Store<uint64_t>(info + 0x10, fi.writeTime);
        Store<uint64_t>(info + 0x18, fi.writeTime);
        Store<uint32_t>(info + 0x20, fi.attributes);
        Store<uint32_t>(info + 0x24, 0);
        written = 0x28;
        break;
    case 5:  // FileStandardInformation
        Store<uint64_t>(info + 0x00, (fi.size + 0xFFF) & ~0xFFFull);
        Store<uint64_t>(info + 0x08, fi.size);
        Store<uint32_t>(info + 0x10, 1);
        Store<uint8_t>(info + 0x14, 0);
        Store<uint8_t>(info + 0x15, fi.directory);
        written = 0x18;
        break;
    case 6:  // FileInternalInformation
        Store<uint64_t>(info, std::hash<std::string>{}(file->path));
        written = 8;
        break;
    case 14:  // FilePositionInformation
        Store<uint64_t>(info, file->position);
        written = 8;
        break;
    case 34:  // FileNetworkOpenInformation
        StoreNetworkOpenInfo(info, fi);
        written = 0x38;
        break;
    case 26:
        // Asked for the utility-partition cache file (cache:\mcache); most likely the physical
        // sector query, which has no meaning for a host file. Xenia rejects it the same way and
        // the title carries on.
        return Return(ctx, status::kInvalidParameter);
    default:
        std::fprintf(stderr, "[io] NtQueryInformationFile class %u unsupported\n", infoClass);
        return Return(ctx, status::kInvalidParameter);
    }
    CompleteIo(Arg32(ctx, 1), status::kSuccess, written, 0);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtSetInformationFile)
{
    // (handle, io_status, info, length, class)
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    const uint32_t info = Arg32(ctx, 2), length = Arg32(ctx, 3), infoClass = Arg32(ctx, 4);
    uint32_t result = status::kSuccess;
    switch (infoClass)
    {
    case 4:  // FileBasicInformation: timestamps/attributes are not persisted
        break;
    case 13:  // FileDispositionInformation: BOOLEAN DeleteFile
        result = file->file->SetDeleteOnClose(Load<uint8_t>(info) != 0);
        if (result != status::kSuccess)
            std::fprintf(stderr, "[io] delete-on-close of %s refused: 0x%08X\n", file->path.c_str(), result);
        break;
    case 14:  // FilePositionInformation
    {
        std::lock_guard lock(file->mutex);
        file->position = Load<uint64_t>(info);
        break;
    }
    case 19:  // FileAllocationInformation
    case 20:  // FileEndOfFileInformation
        result = file->file->SetSize(Load<uint64_t>(info)) ? status::kSuccess : status::kAccessDenied;
        break;
    default:
        std::fprintf(stderr, "[io] NtSetInformationFile class %u unsupported\n", infoClass);
        result = status::kInvalidParameter;
        break;
    }
    CompleteIo(Arg32(ctx, 1), result, result == status::kSuccess ? length : 0, 0);
    Return(ctx, result);
}

PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    // (handle, io_status, info, length, class)
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    const uint32_t info = Arg32(ctx, 2), length = Arg32(ctx, 3), infoClass = Arg32(ctx, 4);
    uint32_t written;
    switch (infoClass)
    {
    case 1:  // FileFsVolumeInformation: creation_time, serial, label_length, supports_objects, label
        Store<uint64_t>(info + 0x00, 0);
        Store<uint32_t>(info + 0x08, 0x48433039);  // "HC09"
        Store<uint32_t>(info + 0x0C, 0);
        Store<uint8_t>(info + 0x10, 0);
        written = 0x12;
        break;
    case 3:  // FileFsSizeInformation: total units, available units, sectors per unit, bytes per sector
        Store<uint64_t>(info + 0x00, 0x200000);
        Store<uint64_t>(info + 0x08, file->path.rfind("\\Device\\Cdrom0", 0) == 0 ? 0 : 0x100000);
        Store<uint32_t>(info + 0x10, 8);
        Store<uint32_t>(info + 0x14, 0x200);
        written = 0x18;
        break;
    case 5:  // FileFsAttributeInformation: attributes, max component length, name length, name
    {
        const char* fsName = "FATX";
        Store<uint32_t>(info + 0x00, 0);
        Store<uint32_t>(info + 0x04, 42);
        Store<uint32_t>(info + 0x08, uint32_t(std::strlen(fsName) * 2));
        for (size_t i = 0; fsName[i] && 0x0C + i * 2 + 2 <= length; i++)
            Store<uint16_t>(info + 0x0C + uint32_t(i) * 2, uint16_t(fsName[i]));
        written = std::min<uint32_t>(length, 0x0C + uint32_t(std::strlen(fsName)) * 2);
        break;
    }
    default:
        std::fprintf(stderr, "[io] NtQueryVolumeInformationFile class %u unsupported\n", infoClass);
        return Return(ctx, status::kInvalidParameter);
    }
    CompleteIo(Arg32(ctx, 1), status::kSuccess, written, 0);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    // (object_attributes, out FILE_NETWORK_OPEN_INFORMATION)
    const std::string path = PathFromAttributes(Arg32(ctx, 0));
    const auto info = QueryFile(path);
    std::fprintf(stderr, "[io] query \"%s\" -> %s\n", path.c_str(), info ? "found" : "not found");
    if (!info)
        return Return(ctx, status::kObjectNameNotFound);
    StoreNetworkOpenInfo(Arg32(ctx, 1), *info);
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    // Xbox signature (no information class or single-entry flag; always one
    // FILE_DIRECTORY_INFORMATION entry per call):
    // (handle, event, apc_routine, apc_context, io_status, info, length, file_name (ANSI_STRING*), restart_scan)
    auto file = LookupHandleAs<FileObject>(Arg32(ctx, 0), ObjectType::File);
    if (!file)
        return Return(ctx, status::kInvalidHandle);
    const uint32_t info = Arg32(ctx, 5), length = Arg32(ctx, 6), patternPtr = Arg32(ctx, 7);
    const bool restart = StackArg32(ctx, 8) != 0;
    const bool single = true;
    std::string pattern = patternPtr ? LoadAnsiString(patternPtr) : std::string();
    if (pattern.empty())
        pattern = "*";

    std::lock_guard lock(file->mutex);
    if (!file->listingLoaded || restart)
    {
        file->listing = file->file->List();
        file->listingIndex = 0;
        file->listingLoaded = true;
    }

    // FILE_DIRECTORY_INFORMATION (Xbox, ANSI name): next_entry_offset, file_index,
    // creation, access, write, change, end_of_file, allocation, attributes, name_length, name[]
    uint32_t offset = 0, previous = 0;
    bool any = false;
    while (file->listingIndex < file->listing.size())
    {
        const auto& entry = file->listing[file->listingIndex];
        if (!WildcardMatch(pattern.c_str(), entry.name.c_str()))
        {
            file->listingIndex++;
            continue;
        }
        const uint32_t size = (0x40 + uint32_t(entry.name.size()) + 7) & ~7u;
        if (offset + 0x40 + entry.name.size() > length)
            break;
        const uint32_t record = info + offset;
        Store<uint32_t>(record + 0x00, 0);
        Store<uint32_t>(record + 0x04, uint32_t(file->listingIndex));
        Store<uint64_t>(record + 0x08, entry.info.creationTime);
        Store<uint64_t>(record + 0x10, entry.info.writeTime);
        Store<uint64_t>(record + 0x18, entry.info.writeTime);
        Store<uint64_t>(record + 0x20, entry.info.writeTime);
        Store<uint64_t>(record + 0x28, entry.info.size);
        Store<uint64_t>(record + 0x30, (entry.info.size + 0xFFF) & ~0xFFFull);
        Store<uint32_t>(record + 0x38, entry.info.attributes);
        Store<uint32_t>(record + 0x3C, uint32_t(entry.name.size()));
        std::memcpy(GuestToHost(record + 0x40), entry.name.data(), entry.name.size());
        if (any)
            Store<uint32_t>(info + previous, offset - previous);
        previous = offset;
        offset += size;
        any = true;
        file->listingIndex++;
        if (single)
            break;
    }
    const uint32_t result = any ? status::kSuccess : status::kNoMoreFiles;
    CompleteIo(Arg32(ctx, 4), result, any ? offset : 0, Arg32(ctx, 1));
    Return(ctx, result);
}

PPC_FUNC(__imp__NtDeviceIoControlFile)
{
    // (handle, event, apc, apc_context, io_status, control_code, in, in_length, out, out_length)
    std::fprintf(stderr, "[io] NtDeviceIoControlFile code 0x%08X unsupported\n", Arg32(ctx, 5));
    Return(ctx, status::kNotSupported);
}

PPC_FUNC(__imp__ObCreateSymbolicLink)
{
    // (link ANSI_STRING*, target ANSI_STRING*)
    const std::string link = LoadAnsiString(Arg32(ctx, 0)), target = LoadAnsiString(Arg32(ctx, 1));
    std::fprintf(stderr, "[io] link \"%s\" -> \"%s\"\n", link.c_str(), target.c_str());
    AddSymbolicLink(link, CanonicalizePath(target));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__ObDeleteSymbolicLink)
{
    RemoveSymbolicLink(LoadAnsiString(Arg32(ctx, 0)));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__IoDismountVolume)
{
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__IoDismountVolumeByFileHandle)
{
    Return(ctx, status::kSuccess);
}
