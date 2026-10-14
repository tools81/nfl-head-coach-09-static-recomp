// XAM users and profiles: one local profile signed in on controller 0.

#include <cstdio>

#include "kernel/guest.h"
#include "kernel/xam.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kSignedInLocally = 1;
        constexpr uint32_t kProfileSettingSize = 0x28;  // X_USER_PROFILE_SETTING

        uint32_t StackArg32(const PPCContext& ctx, int index)
        {
            return Load<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8);
        }

        bool ValidUser(uint32_t userIndex)
        {
            return userIndex == 0;
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__XamUserGetSigninState)
{
    Return(ctx, ValidUser(Arg32(ctx, 0)) ? kSignedInLocally : 0);
}

PPC_FUNC(__imp__XamUserGetXUID)
{
    // (user_index, type_mask, out_xuid)
    if (!ValidUser(Arg32(ctx, 0)))
        return Return(ctx, error::kNoSuchUser);
    Store<uint64_t>(Arg32(ctx, 2), kLocalXuid);
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamUserGetName)
{
    // (user_index, buffer, buffer_length)
    if (!ValidUser(Arg32(ctx, 0)))
        return Return(ctx, error::kNoSuchUser);
    const uint32_t buffer = Arg32(ctx, 1), length = Arg32(ctx, 2);
    if (length == 0)
        return Return(ctx, error::kInsufficientBuffer);
    std::strncpy(reinterpret_cast<char*>(GuestToHost(buffer)), kLocalGamertag, length);
    GuestToHost(buffer)[length - 1] = 0;
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    // (user_index, flags, out X_USER_SIGNIN_INFO):
    // be64 xuid; be32 flags; be32 signin_state; be32 guest_number; be32 sponsor_index; char name[16]
    if (!ValidUser(Arg32(ctx, 0)))
        return Return(ctx, error::kNoSuchUser);
    const uint32_t info = Arg32(ctx, 2);
    std::memset(GuestToHost(info), 0, 0x28);
    Store<uint64_t>(info + 0x00, kLocalXuid);
    Store<uint32_t>(info + 0x0C, kSignedInLocally);
    std::strncpy(reinterpret_cast<char*>(GuestToHost(info + 0x18)), kLocalGamertag, 15);
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    // (user_index, privilege, out_result)
    Store<uint32_t>(Arg32(ctx, 2), 1);
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    // (user_index, xuids, count, out_results, overlapped)
    const uint32_t count = Arg32(ctx, 2), results = Arg32(ctx, 3);
    for (uint32_t i = 0; results && i < count; i++)
        Store<uint32_t>(results + i * 4, 0);
    Return(ctx, CompleteRequest(Arg32(ctx, 4), error::kSuccess));
}

PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    // (title_id, user_index, xuid_count, xuids, setting_count, setting_ids, inout_buffer_size,
    //  buffer, overlapped). Every setting comes back as "no value" (source 0) so the game
    // falls back to its defaults.
    const uint32_t userIndex = Arg32(ctx, 1);
    const uint32_t settingCount = Arg32(ctx, 4), settingIds = Arg32(ctx, 5);
    const uint32_t sizePtr = Arg32(ctx, 6), buffer = Arg32(ctx, 7);
    const uint32_t overlapped = StackArg32(ctx, 8);

    const uint32_t required = 8 + settingCount * kProfileSettingSize;
    if (buffer == 0 || Load<uint32_t>(sizePtr) < required)
    {
        Store<uint32_t>(sizePtr, required);
        return Return(ctx, error::kInsufficientBuffer);
    }

    // X_USER_READ_PROFILE_SETTING_RESULT: be32 count; be32 settings_ptr
    std::memset(GuestToHost(buffer), 0, required);
    Store<uint32_t>(buffer + 0, settingCount);
    Store<uint32_t>(buffer + 4, buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        // X_USER_PROFILE_SETTING: be32 source; pad; be32 user_index (or be64 xuid);
        // be32 setting_id; pad; X_USER_DATA (type byte + payload)
        const uint32_t setting = buffer + 8 + i * kProfileSettingSize;
        Store<uint32_t>(setting + 0x00, 0);
        Store<uint32_t>(setting + 0x08, userIndex);
        Store<uint32_t>(setting + 0x10, Load<uint32_t>(settingIds + i * 4));
    }
    Return(ctx, CompleteRequest(overlapped, error::kSuccess));
}
