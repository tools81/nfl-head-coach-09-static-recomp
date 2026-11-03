// Win32 window + VK_KHR_win32_surface for the Vulkan backend.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include "vk_platform.h"

#include <atomic>

namespace hc09vk {

struct Window
{
    HWND hwnd = nullptr;
    bool closed = false;
    bool escapeCloses = true;
    bool fullscreen = false;
    WINDOWPLACEMENT windowed{ sizeof(WINDOWPLACEMENT) };   // where to return from fullscreen
};

namespace {
const wchar_t* kClass = L"hc09vk";
std::atomic<bool> g_keys[256];   // WM_KEYDOWN/UP state, read by the input thread(s)

void ReleaseAllKeys()
{
    for (auto& k : g_keys) k.store(false, std::memory_order_relaxed);
}

// Borderless fullscreen on the window's monitor, or back to the saved window.
void ApplyFullscreen(Window* w, bool on)
{
    if (on == w->fullscreen) return;
    HWND h = w->hwnd;
    DWORD visible = IsWindowVisible(h) ? WS_VISIBLE : 0;   // a hidden window stays hidden
    if (on)
    {
        GetWindowPlacement(h, &w->windowed);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | visible);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | visible);
        SetWindowPlacement(h, &w->windowed);
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    w->fullscreen = on;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* w = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_CLOSE:
        if (w) w->closed = true;
        return 0;
    case WM_SETCURSOR:
        if (w && w->fullscreen && LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    {
        // Alt+Enter and F11 toggle fullscreen (not on auto-repeat); they never reach the game.
        bool first = !(lp & (1 << 30));
        bool altEnter = msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29));
        if (altEnter || wp == VK_F11)
        {
            if (first && w) ApplyFullscreen(w, !w->fullscreen);
            return 0;
        }
        if (wp < 256) g_keys[wp].store(true, std::memory_order_relaxed);
        if (wp == VK_ESCAPE && w && w->escapeCloses) w->closed = true;
        if (msg == WM_SYSKEYDOWN) break;   // Alt+F4 and the system menu keep working
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp < 256) g_keys[wp].store(false, std::memory_order_relaxed);
        if (msg == WM_SYSKEYUP) break;
        return 0;
    case WM_KILLFOCUS:
        ReleaseAllKeys();   // a key released in another window must not stay held here
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

std::vector<const char*> WindowInstanceExtensions()
{
    return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
}

Window* WindowCreate(const char* title, uint32_t width, uint32_t height, bool show)
{
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered)
    {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kClass;
        // The icon (resource 1, CMakeLists.txt res/hc09-release.ico or res/hc09.ico): the exe's, else this module's
        // (hc09rt.dll in the split build: the player-built hc09.exe has none); none: Windows' default.
        HINSTANCE iconMod = inst;
        if (!FindResourceW(inst, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14)))   // RT_GROUP_ICON
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&WindowCreate), &iconMod);
        wc.hIcon = LoadIconW(iconMod, MAKEINTRESOURCEW(1));
        wc.hIconSm = static_cast<HICON>(LoadImageW(iconMod, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }
    RECT r{ 0, 0, LONG(width), LONG(height) };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    wchar_t wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    auto* w = new Window;
    w->hwnd = CreateWindowExW(0, kClass, wtitle, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!w->hwnd) { delete w; return nullptr; }
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
    if (show) ShowWindow(w->hwnd, SW_SHOW);
    return w;
}

void WindowDestroy(Window* w)
{
    if (!w) return;
    if (w->hwnd) DestroyWindow(w->hwnd);
    delete w;
}

bool WindowPump(Window* w)
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !w->closed;
}

bool WindowWaitEvents(Window* w)
{
    if (!w->closed) WaitMessage();
    return WindowPump(w);
}

void WindowSetEscapeCloses(Window* w, bool closes)
{
    w->escapeCloses = closes;
}

void WindowSetFullscreen(Window* w, bool on)
{
    ApplyFullscreen(w, on);
}

bool WindowKeyDown(uint32_t vk)
{
    return vk < 256 && g_keys[vk].load(std::memory_order_relaxed);
}

void WindowClientSize(Window* w, uint32_t* width, uint32_t* height)
{
    RECT r{};
    GetClientRect(w->hwnd, &r);
    *width = uint32_t(r.right - r.left);
    *height = uint32_t(r.bottom - r.top);
}

VkResult WindowCreateSurface(Window* w, VkInstance instance, VkSurfaceKHR* surface)
{
    VkWin32SurfaceCreateInfoKHR ci{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    ci.hinstance = GetModuleHandleW(nullptr);
    ci.hwnd = w->hwnd;
    return vkCreateWin32SurfaceKHR(instance, &ci, nullptr, surface);
}

}  // namespace hc09vk
