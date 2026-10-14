// XAM/kernel system information: config settings, region, language, launch data,
// execution id, system UI, and RtlRaiseException.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "kernel/guest.h"
#include "kernel/xam.h"
#include "kernel/xex.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kTitleId = 0x45410841;
        constexpr uint32_t kErrorNotFound = 1168;
        constexpr uint32_t kExecutionInfoKey = 0x00040006;
        constexpr uint32_t kSetThreadNameException = 0x406D1388;

        constexpr uint16_t kCategorySecured = 0x0002;
        constexpr uint16_t kCategoryUser = 0x0003;

        uint32_t StackArg32(const PPCContext& ctx, int index)
        {
            return Load<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8);
        }

        // Returns the setting's value width (bytes) and fills `value`, or 0 if unknown.
        uint32_t ConfigSetting(uint16_t category, uint16_t setting, uint32_t& value)
        {
            if (category == kCategorySecured)
            {
                switch (setting)
                {
                case 0x0002: value = 0x00001000; return 4;  // AV region: NTSC-M
                case 0x0003: value = 0x000000FF; return 4;  // game region: North America
                }
            }
            if (category == kCategoryUser)
            {
                switch (setting)
                {
                case 0x0001: value = 0; return 4;           // time zone bias
                case 0x0002: value = 0x50535400; return 4;  // time zone standard name "PST"
                case 0x0003: value = 0x50445400; return 4;  // time zone daylight name "PDT"
                case 0x0004: value = 0; return 4;           // standard date
                case 0x0005: value = 0; return 4;           // daylight date
                case 0x0006: value = 0; return 4;           // time zone standard bias
                case 0x0007: value = 0; return 4;           // time zone daylight bias
                case 0x0009: value = 1; return 4;           // language: English
                case 0x000A: value = 0x00040000; return 4;  // video flags: widescreen
                case 0x000C: value = 0x40; return 4;        // retail flags
                case 0x000E: value = 103; return 1;         // country: United States
                case 0x000F: value = 0; return 4;           // parental controls: off
                }
            }
            return 0;
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__ExGetXConfigSetting)
{
    // (category, setting, buffer, buffer_size, out_required_size)
    const uint16_t category = uint16_t(Arg32(ctx, 0)), setting = uint16_t(Arg32(ctx, 1));
    const uint32_t buffer = Arg32(ctx, 2), bufferSize = Arg32(ctx, 3) & 0xFFFF, requiredPtr = Arg32(ctx, 4);
    uint32_t value = 0;
    const uint32_t width = ConfigSetting(category, setting, value);
    if (width == 0)
    {
        std::fprintf(stderr, "[config] setting %u/0x%X unknown\n", category, setting);
        return Return(ctx, status::kInvalidParameter);
    }
    if (requiredPtr)
        Store<uint16_t>(requiredPtr, uint16_t(width));
    if (buffer)
    {
        if (bufferSize < width)
            return Return(ctx, status::kBufferTooSmall);
        if (width == 1)
            Store<uint8_t>(buffer, uint8_t(value));
        else
            Store<uint32_t>(buffer, value);
    }
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XamGetSystemVersion)
{
    Return(ctx, (2u << 28) | (7357u << 8));  // dashboard 2.0.7357.0
}

PPC_FUNC(__imp__XGetGameRegion)
{
    Return(ctx, 0xFFFF);
}

PPC_FUNC(__imp__XGetLanguage)
{
    Return(ctx, 1);  // English
}

PPC_FUNC(__imp__XGetAVPack)
{
    Return(ctx, 6);  // HDMI
}


PPC_FUNC(__imp__XGetVideoMode)
{
    __imp__VdQueryVideoMode(ctx, base);  // same X_VIDEO_MODE structure
}

PPC_FUNC(__imp__XNetLogonGetTitleID)
{
    Return(ctx, kTitleId);
}

PPC_FUNC(__imp__XNetLogonGetMachineID)
{
    Store<uint64_t>(Arg32(ctx, 0), 0xFA00000000000001ull);
    Return(ctx, 0);
}

PPC_FUNC(__imp__XamGetExecutionId)
{
    // (out_info_ptr): points at the XEX execution info header field
    Store<uint32_t>(Arg32(ctx, 0), XexHeaderField(XexHeaderAddress(), kExecutionInfoKey));
    Return(ctx, status::kSuccess);
}

PPC_FUNC(__imp__XamLoaderGetLaunchDataSize)
{
    Store<uint32_t>(Arg32(ctx, 0), 0);
    Return(ctx, kErrorNotFound);
}

PPC_FUNC(__imp__XamLoaderGetLaunchData)
{
    Return(ctx, kErrorNotFound);
}

PPC_FUNC(__imp__XamLoaderSetLaunchData)
{
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamLoaderLaunchTitle)
{
    const char* name = GuestCString(Arg32(ctx, 0));
    GuestFatal(ctx, "XamLoaderLaunchTitle(\"%s\"): the game asked to launch another executable", name ? name : "");
}

PPC_FUNC(__imp__XamLoaderTerminateTitle)
{
    std::fprintf(stderr, "[xam] title terminated\n");
    std::fflush(stderr);
    std::_Exit(0);
}

PPC_FUNC(__imp__XamEnableInactivityProcessing)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__XamResetInactivity)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__XUsbcamGetState)
{
    Return(ctx, 0);  // no camera
}

PPC_FUNC(__imp__XUsbcamSetConfig)
{
    Return(ctx, 0);
}

PPC_FUNC(__imp__XamShowSigninUI)
{
    // The local profile is always signed in; just report the UI opening and closing.
    BroadcastNotification(kXnSysUi, 1);
    BroadcastNotification(kXnSysUi, 0);
    Return(ctx, error::kSuccess);
}

namespace
{
    // Big-endian UTF-16 guest string as narrow text (non-ASCII shown as '?'), for logging.
    std::string GuestWideText(uint32_t address, size_t limit = 512)
    {
        std::string text;
        if (address == 0)
            return text;
        for (size_t i = 0; i < limit; i++)
        {
            const uint16_t c = Load<uint16_t>(address + uint32_t(i) * 2);
            if (c == 0)
                break;
            text.push_back(c < 0x80 ? char(c) : '?');
        }
        return text;
    }
}

PPC_FUNC(__imp__XamShowMessageBoxUIEx)
{
    // (user_index, title, text, button_count, buttons, focus_button, flags, out_result, overlapped)
    // No system UI: log the message and answer with the focused button at once.
    const uint32_t buttonCount = Arg32(ctx, 3), focus = Arg32(ctx, 5), result = Arg32(ctx, 7);
    const uint32_t chosen = focus < buttonCount ? focus : 0;
    std::fprintf(stderr, "[xam] message box \"%s\": \"%s\" -> button %u\n", GuestWideText(Arg32(ctx, 1)).c_str(),
        GuestWideText(Arg32(ctx, 2)).c_str(), chosen);
    if (result)
        Store<uint32_t>(result, chosen);
    BroadcastNotification(kXnSysUi, 1);
    BroadcastNotification(kXnSysUi, 0);
    Return(ctx, CompleteRequest(StackArg32(ctx, 8), error::kSuccess));
}

PPC_FUNC(__imp__XamShowDirtyDiscErrorUI)
{
    // On a console this shows the disc-error screen and never returns. Here it means a disc
    // read failed: report it loudly and let the title carry on.
    std::fprintf(stderr, "[xam] XamShowDirtyDiscErrorUI: the title reported a disc read error (from 0x%08X)\n",
        uint32_t(ctx.lr));
    Return(ctx, 0);
}

PPC_FUNC(__imp__XamShowDeviceSelectorUI)
{
    // (user_index, content_type, content_flags, total_requested (64-bit), out_device_id, overlapped)
    Store<uint32_t>(Arg32(ctx, 4), 1);  // the hard drive
    Return(ctx, CompleteRequest(Arg32(ctx, 5), error::kSuccess));
}

PPC_FUNC(__imp__RtlRaiseException)
{
    // EXCEPTION_RECORD: be32 code, flags, record, address, parameter_count, parameters[]
    const uint32_t record = Arg32(ctx, 0);
    const uint32_t code = Load<uint32_t>(record);
    if (code == kSetThreadNameException)
    {
        // THREADNAME_INFO: type 0x1000, name, thread id, flags
        const char* name = GuestCString(Load<uint32_t>(record + 0x14 + 4));
        std::fprintf(stderr, "[thread] 0x%X named \"%s\"\n", Load<uint32_t>(record + 0x14 + 8), name ? name : "");
        return;
    }
    std::fprintf(stderr, "[exception] RtlRaiseException(0x%08X) from 0x%08X ignored\n", code, uint32_t(ctx.lr));
}
