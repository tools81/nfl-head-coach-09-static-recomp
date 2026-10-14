// XAM input on top of host XInput. The Xbox 360 XINPUT_* structures match the Windows
// layout except for byte order.
//
// Player 1 also has a virtual pad, merged with any real controller in slot 0 and always
// reported as connected:
//   - keyboard (while the game window has focus): Enter Start, Esc Back, Z/Space A, X B,
//     C X, V Y, Q LB, E RB, 1 LT, 3 RT, F/H left/right stick click, arrows D-pad,
//     WASD left stick, IJKL right stick;
//   - HC09_PRESS=<frame>:<button>[,<frame>:<button>...]: scripted presses (held for six
//     frames), buttons named a b x y lb rb lt rt ls rs up down left right start back.

#include <windows.h>
#include <Xinput.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/draw.h"
#include "kernel/guest.h"

namespace hc09
{
    namespace
    {
        constexpr uint32_t kErrorEmpty = 0x10D2;  // ERROR_EMPTY: no keystroke queued
        constexpr uint32_t kAnyUser = 0xFF;
        constexpr int kPressFrames = 6;

        // XINPUT_GAMEPAD: be16 buttons; u8 left_trigger; u8 right_trigger; be16 lx, ly, rx, ry
        void StoreGamepad(uint32_t address, const XINPUT_GAMEPAD& pad)
        {
            Store<uint16_t>(address + 0, pad.wButtons);
            Store<uint8_t>(address + 2, pad.bLeftTrigger);
            Store<uint8_t>(address + 3, pad.bRightTrigger);
            Store<int16_t>(address + 4, pad.sThumbLX);
            Store<int16_t>(address + 6, pad.sThumbLY);
            Store<int16_t>(address + 8, pad.sThumbRX);
            Store<int16_t>(address + 10, pad.sThumbRY);
        }

        // Pseudo-buttons for the triggers, so they can share the button bitmask.
        constexpr uint32_t kLeftTrigger = 1u << 16, kRightTrigger = 1u << 17;

        struct ButtonName
        {
            const char* name;
            uint32_t mask;
            WORD virtualKey;  // VK_PAD_* keystroke code
        };
        constexpr ButtonName kButtons[] = {
            { "a", XINPUT_GAMEPAD_A, VK_PAD_A },
            { "b", XINPUT_GAMEPAD_B, VK_PAD_B },
            { "x", XINPUT_GAMEPAD_X, VK_PAD_X },
            { "y", XINPUT_GAMEPAD_Y, VK_PAD_Y },
            { "rb", XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
            { "lb", XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER },
            { "lt", kLeftTrigger, VK_PAD_LTRIGGER },
            { "rt", kRightTrigger, VK_PAD_RTRIGGER },
            { "up", XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
            { "down", XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN },
            { "left", XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
            { "right", XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT },
            { "start", XINPUT_GAMEPAD_START, VK_PAD_START },
            { "back", XINPUT_GAMEPAD_BACK, VK_PAD_BACK },
            { "ls", XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
            { "rs", XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
        };

        struct ScriptedPress
        {
            int frame;
            uint32_t mask;
        };

        std::vector<ScriptedPress> ParseScript()
        {
            std::vector<ScriptedPress> presses;
            const char* spec = std::getenv("HC09_PRESS");
            if (!spec)
                return presses;
            std::string text = spec;
            size_t start = 0;
            while (start < text.size())
            {
                size_t end = text.find(',', start);
                if (end == std::string::npos)
                    end = text.size();
                const std::string item = text.substr(start, end - start);
                const size_t colon = item.find(':');
                bool known = false;
                if (colon != std::string::npos)
                {
                    const std::string name = item.substr(colon + 1);
                    for (const ButtonName& button : kButtons)
                    {
                        if (name == button.name)
                        {
                            presses.push_back({ std::atoi(item.c_str()), button.mask });
                            known = true;
                        }
                    }
                }
                if (!known)
                    std::fprintf(stderr, "[input] HC09_PRESS: cannot parse \"%s\"\n", item.c_str());
                start = end + 1;
            }
            return presses;
        }

        bool GameWindowFocused()
        {
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            return process == GetCurrentProcessId();
        }

        bool KeyDown(int key)
        {
            return (GetAsyncKeyState(key) & 0x8000) != 0;
        }

        int16_t Axis(int negative, int positive)
        {
            return int16_t((KeyDown(positive) ? 32767 : 0) - (KeyDown(negative) ? 32767 : 0));
        }

        struct VirtualPad
        {
            std::mutex mutex;
            std::vector<ScriptedPress> script = ParseScript();
            uint32_t previousButtons = 0;
            DWORD packetNumber = 0;
            XINPUT_GAMEPAD previousPad{};
            std::deque<XINPUT_KEYSTROKE> keystrokes;
        };

        VirtualPad& Pad()
        {
            static VirtualPad pad;
            return pad;
        }

        // Samples the virtual pad (keyboard + script) as buttons and sticks.
        XINPUT_GAMEPAD SampleVirtual(uint32_t& buttons)
        {
            XINPUT_GAMEPAD pad{};
            buttons = 0;
            const int frame = gpu::FrameNumber();
            for (const ScriptedPress& press : Pad().script)
                if (frame >= press.frame && frame < press.frame + kPressFrames)
                    buttons |= press.mask;
            if (GameWindowFocused())
            {
                const struct { int key; uint32_t mask; } keys[] = {
                    { VK_RETURN, XINPUT_GAMEPAD_START }, { VK_ESCAPE, XINPUT_GAMEPAD_BACK },
                    { 'Z', XINPUT_GAMEPAD_A }, { VK_SPACE, XINPUT_GAMEPAD_A }, { 'X', XINPUT_GAMEPAD_B },
                    { 'C', XINPUT_GAMEPAD_X }, { 'V', XINPUT_GAMEPAD_Y },
                    { 'Q', XINPUT_GAMEPAD_LEFT_SHOULDER }, { 'E', XINPUT_GAMEPAD_RIGHT_SHOULDER },
                    { '1', kLeftTrigger }, { '3', kRightTrigger },
                    { 'F', XINPUT_GAMEPAD_LEFT_THUMB }, { 'H', XINPUT_GAMEPAD_RIGHT_THUMB },
                    { VK_UP, XINPUT_GAMEPAD_DPAD_UP }, { VK_DOWN, XINPUT_GAMEPAD_DPAD_DOWN },
                    { VK_LEFT, XINPUT_GAMEPAD_DPAD_LEFT }, { VK_RIGHT, XINPUT_GAMEPAD_DPAD_RIGHT },
                };
                for (const auto& key : keys)
                    if (KeyDown(key.key))
                        buttons |= key.mask;
                pad.sThumbLX = Axis('A', 'D');
                pad.sThumbLY = Axis('S', 'W');
                pad.sThumbRX = Axis('J', 'L');
                pad.sThumbRY = Axis('K', 'I');
            }
            return pad;
        }

        // Player 1's merged state; false when it is not user 0. Also queues keystroke events
        // for virtual button transitions.
        bool MergedState(uint32_t user, XINPUT_STATE& state)
        {
            if (user != 0)
                return false;
            VirtualPad& pad = Pad();
            std::lock_guard lock(pad.mutex);
            XINPUT_STATE host{};
            const bool connected = XInputGetState(0, &host) == ERROR_SUCCESS;
            uint32_t buttons = 0;
            XINPUT_GAMEPAD merged = SampleVirtual(buttons);
            if (connected)
            {
                merged.wButtons = host.Gamepad.wButtons;
                merged.bLeftTrigger = host.Gamepad.bLeftTrigger;
                merged.bRightTrigger = host.Gamepad.bRightTrigger;
                if (!merged.sThumbLX && !merged.sThumbLY)
                    merged.sThumbLX = host.Gamepad.sThumbLX, merged.sThumbLY = host.Gamepad.sThumbLY;
                if (!merged.sThumbRX && !merged.sThumbRY)
                    merged.sThumbRX = host.Gamepad.sThumbRX, merged.sThumbRY = host.Gamepad.sThumbRY;
            }
            merged.wButtons |= WORD(buttons);
            if (buttons & kLeftTrigger)
                merged.bLeftTrigger = 255;
            if (buttons & kRightTrigger)
                merged.bRightTrigger = 255;

            // Keystrokes come from the virtual buttons only; real controllers report through
            // XInputGetKeystroke.
            const uint32_t changed = buttons ^ pad.previousButtons;
            for (const ButtonName& button : kButtons)
            {
                if (!(changed & button.mask))
                    continue;
                XINPUT_KEYSTROKE stroke{};
                stroke.VirtualKey = button.virtualKey;
                stroke.Flags = (buttons & button.mask) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
                stroke.UserIndex = 0;
                pad.keystrokes.push_back(stroke);
                if (buttons & button.mask)
                    std::fprintf(stderr, "[input] frame %d: %s\n", gpu::FrameNumber(), button.name);
            }
            pad.previousButtons = buttons;
            if (std::memcmp(&merged, &pad.previousPad, sizeof(merged)) != 0)
                pad.packetNumber++;
            pad.previousPad = merged;
            state.dwPacketNumber = pad.packetNumber + (connected ? host.dwPacketNumber : 0);
            state.Gamepad = merged;
            return true;
        }
    }
}

using namespace hc09;

PPC_FUNC(__imp__XamInputGetCapabilities)
{
    // (user_index, flags, out XINPUT_CAPABILITIES: u8 type; u8 subtype; be16 flags;
    //  XINPUT_GAMEPAD gamepad; be16 left_motor; be16 right_motor)
    const uint32_t user = Arg32(ctx, 0), caps = Arg32(ctx, 2);
    XINPUT_CAPABILITIES host{};
    if (user >= XUSER_MAX_COUNT)
        return Return(ctx, error::kDeviceNotConnected);
    if (XInputGetCapabilities(user, 0, &host) != ERROR_SUCCESS)
    {
        if (user != 0)
            return Return(ctx, error::kDeviceNotConnected);
        // The virtual pad: a standard gamepad with every control present.
        host.Type = XINPUT_DEVTYPE_GAMEPAD;
        host.SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        host.Gamepad = { 0xF3FF, 255, 255, -1, -1, -1, -1 };
    }
    Store<uint8_t>(caps + 0, host.Type);
    Store<uint8_t>(caps + 1, host.SubType);
    Store<uint16_t>(caps + 2, host.Flags);
    StoreGamepad(caps + 4, host.Gamepad);
    Store<uint16_t>(caps + 16, host.Vibration.wLeftMotorSpeed);
    Store<uint16_t>(caps + 18, host.Vibration.wRightMotorSpeed);
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamInputGetState)
{
    // (user_index, flags, out XINPUT_STATE: be32 packet_number; XINPUT_GAMEPAD gamepad)
    const uint32_t user = Arg32(ctx, 0), state = Arg32(ctx, 2);
    XINPUT_STATE host{};
    if (user >= XUSER_MAX_COUNT || (!MergedState(user, host) && XInputGetState(user, &host) != ERROR_SUCCESS))
        return Return(ctx, error::kDeviceNotConnected);
    if (state)
    {
        Store<uint32_t>(state, host.dwPacketNumber);
        StoreGamepad(state + 4, host.Gamepad);
    }
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamInputSetState)
{
    // (user_index, unknown, XINPUT_VIBRATION: be16 left_motor; be16 right_motor)
    const uint32_t user = Arg32(ctx, 0), vibration = Arg32(ctx, 2);
    XINPUT_VIBRATION host{ Load<uint16_t>(vibration), Load<uint16_t>(vibration + 2) };
    if (user >= XUSER_MAX_COUNT)
        return Return(ctx, error::kDeviceNotConnected);
    if (XInputSetState(user, &host) != ERROR_SUCCESS && user != 0)
        return Return(ctx, error::kDeviceNotConnected);
    Return(ctx, error::kSuccess);
}

PPC_FUNC(__imp__XamInputGetKeystrokeEx)
{
    // (inout_user_index, flags, out XINPUT_KEYSTROKE: be16 vk; be16 unicode; be16 flags;
    //  u8 user_index; u8 hid_code)
    const uint32_t userPtr = Arg32(ctx, 0), keystroke = Arg32(ctx, 2);
    const uint32_t requested = Load<uint32_t>(userPtr);
    XINPUT_KEYSTROKE host{};
    DWORD result = ERROR_EMPTY;
    if (requested == 0 || requested == kAnyUser)
    {
        XINPUT_STATE ignored;
        MergedState(0, ignored);  // samples the virtual pad and queues its transitions
        VirtualPad& pad = Pad();
        std::lock_guard lock(pad.mutex);
        if (!pad.keystrokes.empty())
        {
            host = pad.keystrokes.front();
            pad.keystrokes.pop_front();
            result = ERROR_SUCCESS;
        }
    }
    if (result == ERROR_EMPTY)
        result = XInputGetKeystroke(requested == kAnyUser ? XUSER_INDEX_ANY : requested, 0, &host);
    if (result == ERROR_EMPTY)
        return Return(ctx, kErrorEmpty);
    if (result != ERROR_SUCCESS)
        return Return(ctx, error::kDeviceNotConnected);
    Store<uint16_t>(keystroke + 0, host.VirtualKey);
    Store<uint16_t>(keystroke + 2, uint16_t(host.Unicode));
    Store<uint16_t>(keystroke + 4, host.Flags);
    Store<uint8_t>(keystroke + 6, host.UserIndex);
    Store<uint8_t>(keystroke + 7, host.HidCode);
    Store<uint32_t>(userPtr, host.UserIndex);
    Return(ctx, error::kSuccess);
}
