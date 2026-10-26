#include "gpu/swappable_images.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <unordered_map>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu/command_processor.h"
#include "kernel/guest.h"

// Ion::SwappableImageManager (the code around 0x82EF0000-0x82F08000):
//   sub_82EFC0F8(context, library, id) -> the manager of that library
//   sub_82EFE308(manager, id, entry, ...) starts loading an image (asynchronously)
//   sub_82F05DB0(image, ...) creates the texture once the image's XPR2 data has been read:
//     image+0x34 is the D3D texture (its fetch constant starts at +0x1C, so +0x20 holds the
//     base address and format), image+0x3C the image id.
extern "C" PPC_FUNC(__imp__sub_82EFC0F8);
extern "C" PPC_FUNC(__imp__sub_82EFE308);
extern "C" PPC_FUNC(__imp__sub_82F05DB0);

namespace hc09::gpu
{
    namespace
    {
        // Library index -> name, in the order of the headcoach/swappables/... strings in the
        // executable (index 3, PlayerPortraits, confirmed by tracing a portrait load).
        const char* const kLibraries[] = {
            "loading", "LoadingCampus", "largeteamlogos", "PlayerPortraits", "CoachPortraits", "helmets",
            "SkillIcons", "CreatePlayIcons", "HallOfFameCoaches", "TeamSelect", "DefiningMoments",
            "TeamLogosSmall", "lobbycategory", "PlayerModuleIcons", "PipBackground", "WizardIcons",
            "Tornadoes", "TeamModuleIcons", "Glossary360", "GlossaryPS3", "TeamLogosExtraSmall",
            "TeamSelectShadows",
        };
        constexpr uint32_t kCheckedBytes = 4096;  // checksum of the start of the data

        struct Registered
        {
            SwappableImage image;
            uint64_t checksum = 0;
        };

        std::mutex g_mutex;
        std::unordered_map<uint32_t, uint32_t> g_managerLibrary;      // manager -> library index
        std::deque<std::pair<uint32_t, uint32_t>> g_pending;           // (library, id) being loaded
        std::unordered_map<uint32_t, Registered> g_textures;           // physical base -> image
        const bool g_trace = std::getenv("HC09_TRACE_SWAPPABLES") != nullptr;

        uint64_t Checksum(uint32_t physical)
        {
            return XXH3_64bits(GuestToHost(PhysicalToGuest(physical)), kCheckedBytes);
        }
    }

    std::optional<SwappableImage> FindSwappableImage(uint32_t physical)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_textures.find(physical);
        if (it == g_textures.end())
            return std::nullopt;
        if (Checksum(physical) != it->second.checksum)
        {
            g_textures.erase(it);  // the memory now holds something else
            return std::nullopt;
        }
        return it->second.image;
    }
}

using namespace hc09;
using namespace hc09::gpu;

PPC_FUNC(sub_82EFC0F8)
{
    const uint32_t library = ctx.r4.u32;
    __imp__sub_82EFC0F8(ctx, base);
    if (ctx.r3.u32 && library < std::size(kLibraries))
    {
        std::lock_guard lock(g_mutex);
        g_managerLibrary[ctx.r3.u32] = library;
    }
}

PPC_FUNC(sub_82EFE308)
{
    const uint32_t manager = ctx.r3.u32, id = ctx.r4.u32;
    __imp__sub_82EFE308(ctx, base);
    std::lock_guard lock(g_mutex);
    auto it = g_managerLibrary.find(manager);
    if (it == g_managerLibrary.end())
        return;
    g_pending.emplace_back(it->second, id);
    if (g_pending.size() > 256)
        g_pending.pop_front();
}

PPC_FUNC(sub_82F05DB0)
{
    const uint32_t image = ctx.r3.u32;
    __imp__sub_82F05DB0(ctx, base);
    const uint32_t id = Load<uint32_t>(image + 0x3C);
    const uint32_t texture = Load<uint32_t>(image + 0x34);
    if (texture == 0)
        return;
    const uint32_t physical = Load<uint32_t>(texture + 0x20) & 0x1FFFF000;
    std::lock_guard lock(g_mutex);
    // The oldest load of this id names the library (ids repeat across libraries).
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it)
    {
        if (it->second != id)
            continue;
        Registered entry{ { kLibraries[it->first], id }, Checksum(physical) };
        g_pending.erase(it);
        if (g_trace)
            std::fprintf(stderr, "[swappable] %s/%u -> texture 0x%08X\n", entry.image.library.c_str(), id, physical);
        g_textures[physical] = std::move(entry);
        return;
    }
    if (g_trace)
        std::fprintf(stderr, "[swappable] image %u at 0x%08X created without a known load\n", id, physical);
}
