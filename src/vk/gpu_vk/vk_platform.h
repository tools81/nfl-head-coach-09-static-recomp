#pragma once
// Thin platform layer for the Vulkan backend: one window with a Vulkan
// surface. Win32 today (vk_platform_win32.cpp); an Xlib/Wayland version for
// WSLg can sit beside it with the same functions. Events are handled on the
// thread that created the window.
#include <cstdint>
#include <vector>

// Vulkan through volk (this project): vulkan-1.dll is loaded at run time, so no Vulkan SDK is needed to build.
#include "volk.h"

namespace hc09vk {

struct Window;

// Instance extensions the window's surface needs (VK_KHR_surface + platform).
std::vector<const char*> WindowInstanceExtensions();
// A window whose client area is width x height pixels; nullptr on failure.
// `show` = false: never shown (tests of the present path while the desktop is in use).
Window* WindowCreate(const char* title, uint32_t width, uint32_t height, bool show = true);
void WindowDestroy(Window* w);
// Handle pending window events; false once the user closed the window.
bool WindowPump(Window* w);
// Block until window events arrive, then handle them (a thread that owns the
// window and does nothing else); false once the user closed the window.
bool WindowWaitEvents(Window* w);
// Esc closes the window (default: yes — viewers; the game wants the key).
void WindowSetEscapeCloses(Window* w, bool closes);
// Borderless fullscreen on the window's monitor (or back). Alt+Enter and F11
// toggle it too. Call on the window's thread.
void WindowSetFullscreen(Window* w, bool on);
void WindowClientSize(Window* w, uint32_t* width, uint32_t* height);
// Whether a key (Win32 virtual-key code, 0-255) is held in a window of this
// process; every key reads as up while none of them has focus. Any thread.
bool WindowKeyDown(uint32_t vk);
VkResult WindowCreateSurface(Window* w, VkInstance instance, VkSurfaceKHR* surface);

}  // namespace hc09vk
