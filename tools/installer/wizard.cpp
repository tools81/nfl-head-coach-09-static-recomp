// HC09Setup.exe: Wizard97-style installer for the NFL Head Coach 09 recompilation.
//
//   HC09Setup.exe                    install (welcome, requirements, location, disc image, options,
//                                    ready, progress, finish)
//   HC09Setup.exe --update <folder>  update an existing install to the latest release (started by
//                                    HC09.exe when automatic update checks are enabled)

#include <windows.h>
#include <commctrl.h>
#include <prsht.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "common/settings.h"
#include "common/updates.h"
#include "disc.h"
#include "pipeline.h"
#include "resource.h"
#include "xex.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "              \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace hc09;
using namespace hc09::setup;

namespace
{
    constexpr UINT WM_APP_STEP = WM_APP + 1;  // wParam step, lParam: std::wstring* title (owned)
    constexpr UINT WM_APP_LOG = WM_APP + 2;   // lParam: std::wstring* line (owned)
    constexpr UINT WM_APP_DONE = WM_APP + 3;  // wParam: success
    constexpr UINT WM_APP_NEXT_TEXT = WM_APP + 4;  // relabel Next once the page is active

    constexpr wchar_t kProduct[] = L"NFL Head Coach 09 Recompilation";
    constexpr size_t kMaxRootLength = 120;  // install folder path, in characters

    struct App
    {
        HINSTANCE instance = nullptr;
        bool updateMode = false;
        std::wstring targetRelease;  // update mode: the release being installed
        Toolchain toolchain;
        std::vector<Requirement> requirements;
        InstallOptions options;
        std::unique_ptr<Pipeline> pipeline;
        std::thread worker;
        bool running = false;
        bool succeeded = false;
        bool isoValid = false;
        HFONT titleFont = nullptr;
    } g;

    // ---- Small helpers --------------------------------------------------------------------------

    std::wstring GetText(HWND dialog, int id)
    {
        HWND control = GetDlgItem(dialog, id);
        std::wstring text(size_t(GetWindowTextLengthW(control)), L'\0');
        GetWindowTextW(control, text.data(), int(text.size()) + 1);
        return text;
    }

    void SetResult(HWND dialog, LONG_PTR result)
    {
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
    }

    void SetButtons(HWND page, DWORD buttons)
    {
        PropSheet_SetWizButtons(GetParent(page), buttons);
    }

    // Relabels the wizard's Next button (PSM_SETNEXTTEXT only works for Aero-style wizards).
    void SetNextLabel(HWND page, const wchar_t* text)
    {
        constexpr int kNextButton = 0x3024;
        SetWindowTextW(GetDlgItem(GetParent(page), kNextButton), text);
    }

    std::wstring FormatGigabytes(uint64_t bytes)
    {
        wchar_t text[32];
        swprintf(text, std::size(text), L"%.1f GB", double(bytes) / double(1ull << 30));
        return text;
    }

    void Open(const std::wstring& url)
    {
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::filesystem::path ExecutablePath()
    {
        wchar_t path[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return std::filesystem::path(std::wstring(path, length));
    }

    std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR path = nullptr;
        std::filesystem::path result;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)))
            result = path;
        CoTaskMemFree(path);
        return result;
    }

    // Browse for a folder (folder == true) or an .iso file with the common item dialog.
    std::wstring Browse(HWND owner, bool folder, const std::wstring& initial)
    {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return {};
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
        if (!folder)
        {
            const COMDLG_FILTERSPEC filters[] = { { L"Disc images (*.iso)", L"*.iso" }, { L"All files", L"*.*" } };
            dialog->SetFileTypes(UINT(std::size(filters)), filters);
            dialog->SetTitle(L"Select your NFL Head Coach 09 disc image");
        }
        else
        {
            dialog->SetTitle(L"Select the install folder");
        }
        std::error_code ec;
        std::filesystem::path start = initial;
        while (!start.empty() && !std::filesystem::is_directory(start, ec))
            start = start.parent_path();
        IShellItem* item = nullptr;
        if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&item))))
        {
            dialog->SetFolder(item);
            item->Release();
        }
        std::wstring result;
        if (SUCCEEDED(dialog->Show(owner)) && SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                result = path;
            CoTaskMemFree(path);
            item->Release();
        }
        dialog->Release();
        return result;
    }

    // Wizard97 artwork, drawn at startup: the exterior pages' watermark and the header icon.
    HBITMAP CreateArtwork(int width, int height, bool watermark)
    {
        BITMAPINFO info{};
        info.bmiHeader = { sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!bitmap)
            return nullptr;
        HGDIOBJ previous = SelectObject(dc, bitmap);
        // Navy to royal blue, with white yard lines.
        auto* pixels = static_cast<uint32_t*>(bits);
        for (int y = 0; y < height; y++)
        {
            const double t = double(y) / std::max(height - 1, 1);
            const uint32_t r = uint32_t(0x0B + t * (0x1F - 0x0B)), gr = uint32_t(0x1E + t * (0x4E - 0x1E)), b = uint32_t(0x4D + t * (0xA8 - 0x4D));
            for (int x = 0; x < width; x++)
                pixels[y * width + x] = (r << 16) | (gr << 8) | b;
        }
        const int spacing = watermark ? 28 : 12;
        for (int y = spacing / 2; y < height; y += spacing)
            for (int x = 0; x < width; x++)
                pixels[y * width + x] = 0x3A62B0;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HFONT big = CreateFontW(watermark ? -46 : -22, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, big);
        RECT area{ 0, 0, width, height };
        if (watermark)
        {
            RECT top{ 0, 40, width, 150 };
            DrawTextW(dc, L"HC\n09", -1, &top, DT_CENTER | DT_NOPREFIX);
            HFONT caption = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            SelectObject(dc, caption);
            RECT bottom{ 0, height - 60, width, height - 20 };
            DrawTextW(dc, L"STATIC\nRECOMPILATION", -1, &bottom, DT_CENTER | DT_NOPREFIX);
            SelectObject(dc, big);
            DeleteObject(caption);
        }
        else
        {
            DrawTextW(dc, L"09", -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
        DeleteObject(big);
        SelectObject(dc, previous);
        DeleteDC(dc);
        return bitmap;
    }

    // The Wizard97 exterior-page title font (bold, larger than the dialog font).
    void ApplyTitleFont(HWND dialog)
    {
        if (!g.titleFont)
        {
            HDC dc = GetDC(dialog);
            const int height = -MulDiv(12, GetDeviceCaps(dc, LOGPIXELSY), 72);
            ReleaseDC(dialog, dc);
            g.titleFont = CreateFontW(height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Verdana");
        }
        SendDlgItemMessageW(dialog, IDC_TITLE, WM_SETFONT, WPARAM(g.titleFont), TRUE);
    }

    bool RequirementsMet()
    {
        for (const auto& requirement : g.requirements)
            if (!requirement.met)
                return false;
        return true;
    }

    // Checks that the file is an NFL Head Coach 09 disc image; `status` explains the result.
    bool CheckIso(const std::filesystem::path& iso, std::wstring& status)
    {
        std::error_code ec;
        if (iso.empty())
        {
            status = L"Choose the disc image file.";
            return false;
        }
        if (!std::filesystem::is_regular_file(iso, ec))
        {
            status = L"The file does not exist.";
            return false;
        }
        std::wstring error;
        const auto file = FindDiscFile(iso, "default.xex", error);
        if (!file)
        {
            status = error;
            return false;
        }
        std::ifstream input(iso, std::ios::binary);
        std::vector<uint8_t> header(size_t(std::min<uint64_t>(file->size, 0x10000)));
        input.seekg(std::streamoff(file->offset));
        input.read(reinterpret_cast<char*>(header.data()), std::streamsize(header.size()));
        const auto title = XexTitleId(header);
        if (!title)
        {
            status = L"The disc's default.xex is not an Xbox 360 executable.";
            return false;
        }
        if (*title != kHeadCoach09TitleId)
        {
            wchar_t text[160];
            swprintf(text, std::size(text), L"This is a different game (title ID %08X); NFL Head Coach 09 is 45410841.", *title);
            status = text;
            return false;
        }
        status = L"\x2714 NFL Head Coach 09 (title 45410841) found. " + FormatGigabytes(std::filesystem::file_size(iso, ec)) + L" image.";
        return true;
    }

    // ---- Pages --------------------------------------------------------------------------------

    INT_PTR CALLBACK WelcomePage(HWND dialog, UINT message, WPARAM, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            ApplyTitleFont(dialog);
            SetDlgItemTextW(dialog, IDC_TITLE, L"Welcome to the NFL Head Coach 09 Recompilation Setup Wizard");
            SetDlgItemTextW(dialog, IDC_TEXT,
                L"This wizard installs a native Windows version of NFL Head Coach 09 (Xbox 360).\n\n"
                L"No game data is included. You need your own disc image (.iso) of the game: the game is imported from it "
                L"and rebuilt on this PC. Building needs Visual Studio (with its C++, Clang and CMake tools) and Git; "
                L"the next page checks for them.\n\n"
                L"Installing downloads about 1 GB and takes roughly 10 to 20 minutes.\n\n"
                L"Click Next to continue.");
            return TRUE;
        }
        if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lParam)->code == PSN_SETACTIVE)
        {
            SetButtons(dialog, PSWIZB_NEXT);
            return TRUE;
        }
        return FALSE;
    }

    void FillRequirements(HWND dialog)
    {
        HWND list = GetDlgItem(dialog, IDC_REQ_LIST);
        ListView_DeleteAllItems(list);
        for (int i = 0; i < int(g.requirements.size()); i++)
        {
            const Requirement& requirement = g.requirements[i];
            LVITEMW item{ LVIF_TEXT };
            item.iItem = i;
            std::wstring name = (requirement.met ? L"\x2714  " : L"\x2716  ") + requirement.name;
            item.pszText = name.data();
            ListView_InsertItem(list, &item);
            std::wstring detail = requirement.detail;
            ListView_SetItemText(list, i, 1, detail.data());
        }
        EnableWindow(GetDlgItem(dialog, IDC_REQ_HELP), !RequirementsMet());
        SetDlgItemTextW(dialog, IDC_REQ_TEXT, RequirementsMet()
            ? L"Everything needed to build the game is installed."
            : L"Install the missing items (select one and click Get), then click Check again.");
    }

    INT_PTR CALLBACK RequirementsPage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
        {
            HWND list = GetDlgItem(dialog, IDC_REQ_LIST);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
            RECT area;
            GetClientRect(list, &area);
            LVCOLUMNW column{ LVCF_TEXT | LVCF_WIDTH };
            column.pszText = const_cast<wchar_t*>(L"Requirement");
            column.cx = (area.right - area.left) * 45 / 100;
            ListView_InsertColumn(list, 0, &column);
            column.pszText = const_cast<wchar_t*>(L"Status");
            column.cx = (area.right - area.left) - column.cx - GetSystemMetrics(SM_CXVSCROLL);
            ListView_InsertColumn(list, 1, &column);
            FillRequirements(dialog);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_REQ_RECHECK)
            {
                SetCursor(LoadCursor(nullptr, IDC_WAIT));
                g.requirements = CheckRequirements(g.toolchain);
                FillRequirements(dialog);
                SetButtons(dialog, PSWIZB_BACK | (RequirementsMet() ? PSWIZB_NEXT : 0));
            }
            else if (LOWORD(wParam) == IDC_REQ_HELP)
            {
                const int selected = ListView_GetNextItem(GetDlgItem(dialog, IDC_REQ_LIST), -1, LVNI_SELECTED);
                for (int i = 0; i < int(g.requirements.size()); i++)
                {
                    if ((selected < 0 || selected == i) && !g.requirements[i].helpUrl.empty())
                    {
                        Open(g.requirements[i].helpUrl);
                        break;
                    }
                }
            }
            return TRUE;
        case WM_NOTIFY:
        {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->code == PSN_SETACTIVE)
            {
                SetButtons(dialog, (g.updateMode ? 0 : PSWIZB_BACK) | (RequirementsMet() ? PSWIZB_NEXT : 0));
                return TRUE;
            }
            if (header->idFrom == IDC_REQ_LIST && header->code == NM_DBLCLK)
            {
                const int item = reinterpret_cast<const NMITEMACTIVATE*>(lParam)->iItem;
                if (item >= 0 && item < int(g.requirements.size()) && !g.requirements[item].helpUrl.empty())
                    Open(g.requirements[item].helpUrl);
                return TRUE;
            }
            return FALSE;
        }
        }
        return FALSE;
    }

    void DescribeLocation(HWND dialog)
    {
        const std::filesystem::path folder = GetText(dialog, IDC_LOC_EDIT);
        std::wstring info;
        std::error_code ec;
        if (folder.empty() || !folder.is_absolute())
            info = L"Enter a full folder path, such as C:\\Games\\HC09Recomp.";
        else if (IsProjectFolder(folder))
            info = L"This folder already holds the project's source; it will be used as it is.";
        else if (std::filesystem::exists(folder, ec) && !std::filesystem::is_empty(folder, ec))
            info = L"This folder is not empty. Choose a new or empty folder.";
        else
            info = L"The project will be downloaded into this folder.";
        // The build tools nest folders deeply; long base paths can exceed Windows' path limit.
        if (folder.wstring().size() > kMaxRootLength)
            info = L"This path is too long for the build tools. Choose a shorter one, such as C:\\Games\\HC09Recomp.";
        else if (folder.wstring().size() > 80)
            info += L" A shorter path (such as C:\\Games\\HC09Recomp) is safer for the build tools.";
        info += L"\n\nRequired: about " + FormatGigabytes(kRequiredSpace) + L". Available: " + FormatGigabytes(FreeSpace(folder)) + L".";
        SetDlgItemTextW(dialog, IDC_LOC_INFO, info.c_str());
    }

    INT_PTR CALLBACK LocationPage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
            SetDlgItemTextW(dialog, IDC_TEXT,
                L"The source code, the build tools and the rebuilt game are kept in this folder. Choose a new or empty "
                L"folder, or an existing copy of the project.");
            SetDlgItemTextW(dialog, IDC_LOC_EDIT, g.options.root.c_str());
            DescribeLocation(dialog);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_LOC_BROWSE)
            {
                const std::wstring folder = Browse(GetParent(dialog), true, GetText(dialog, IDC_LOC_EDIT));
                if (!folder.empty())
                    SetDlgItemTextW(dialog, IDC_LOC_EDIT, folder.c_str());
            }
            else if (LOWORD(wParam) == IDC_LOC_EDIT && HIWORD(wParam) == EN_CHANGE)
            {
                DescribeLocation(dialog);
            }
            return TRUE;
        case WM_NOTIFY:
            switch (reinterpret_cast<NMHDR*>(lParam)->code)
            {
            case PSN_SETACTIVE:
                SetButtons(dialog, PSWIZB_BACK | PSWIZB_NEXT);
                return TRUE;
            case PSN_WIZNEXT:
            {
                const std::filesystem::path folder = GetText(dialog, IDC_LOC_EDIT);
                std::error_code ec;
                const wchar_t* problem = nullptr;
                if (folder.empty() || !folder.is_absolute())
                    problem = L"Enter a full folder path.";
                // XenonRecomp's submodules (.git\modules\...) and build objects add ~140 characters, and
                // git cannot fetch into paths past Windows' 260-character limit.
                else if (folder.wstring().size() > kMaxRootLength)
                    problem = L"The folder path is too long for the build tools. Choose a shorter one, such as C:\\Games\\HC09Recomp.";
                else if (!IsProjectFolder(folder) && std::filesystem::exists(folder, ec) && !std::filesystem::is_empty(folder, ec))
                    problem = L"The folder is not empty. Choose a new or empty folder, or an existing copy of the project.";
                else if (FreeSpace(folder) < kRequiredSpace &&
                    MessageBoxW(GetParent(dialog), L"The drive may not have enough free space. Continue anyway?", kProduct,
                        MB_YESNO | MB_ICONWARNING) != IDYES)
                    problem = L"";
                if (problem)
                {
                    if (*problem)
                        MessageBoxW(GetParent(dialog), problem, kProduct, MB_OK | MB_ICONWARNING);
                    SetResult(dialog, -1);
                    return TRUE;
                }
                g.options.root = folder.lexically_normal();
                return TRUE;
            }
            }
            return FALSE;
        }
        return FALSE;
    }

    void ValidateIso(HWND dialog)
    {
        std::wstring status;
        g.isoValid = CheckIso(GetText(dialog, IDC_ISO_EDIT), status);
        SetDlgItemTextW(dialog, IDC_ISO_STATUS, status.c_str());
        SetButtons(dialog, PSWIZB_BACK | (g.isoValid ? PSWIZB_NEXT : 0));
    }

    INT_PTR CALLBACK IsoPage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
            SetDlgItemTextW(dialog, IDC_TEXT,
                L"Select your NFL Head Coach 09 (Xbox 360) disc image. The game's executable is imported from it now, and the "
                L"game reads its data straight from it while you play, so keep the file where it is.");
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_ISO_BROWSE)
            {
                const std::wstring file = Browse(GetParent(dialog), false, GetText(dialog, IDC_ISO_EDIT));
                if (!file.empty())
                    SetDlgItemTextW(dialog, IDC_ISO_EDIT, file.c_str());
            }
            else if (LOWORD(wParam) == IDC_ISO_EDIT && HIWORD(wParam) == EN_CHANGE)
            {
                ValidateIso(dialog);
            }
            return TRUE;
        case WM_NOTIFY:
            switch (reinterpret_cast<NMHDR*>(lParam)->code)
            {
            case PSN_SETACTIVE:
                // Prefill from an earlier install, or an image in the project's iso folder.
                if (GetText(dialog, IDC_ISO_EDIT).empty())
                {
                    std::wstring iso = LoadSettings(g.options.root).isoPath;
                    std::error_code ec;
                    if (iso.empty() && std::filesystem::is_directory(g.options.root / L"iso", ec))
                        for (const auto& entry : std::filesystem::directory_iterator(g.options.root / L"iso", ec))
                            if (_wcsicmp(entry.path().extension().c_str(), L".iso") == 0)
                                iso = entry.path().wstring();
                    SetDlgItemTextW(dialog, IDC_ISO_EDIT, iso.c_str());
                }
                ValidateIso(dialog);
                return TRUE;
            case PSN_WIZNEXT:
                if (!g.isoValid)
                {
                    SetResult(dialog, -1);
                    return TRUE;
                }
                g.options.iso = GetText(dialog, IDC_ISO_EDIT);
                return TRUE;
            }
            return FALSE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK OptionsPage(HWND dialog, UINT message, WPARAM, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
            SetDlgItemTextW(dialog, IDC_OPT_UPDATES_TEXT,
                L"When enabled, the game checks GitHub each time it starts. If a newer release is available it asks first, "
                L"then downloads the new source and rebuilds the game (a few minutes). You can change this later in hc09.ini.");
            CheckDlgButton(dialog, IDC_OPT_UPDATES, g.options.autoUpdate ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(dialog, IDC_OPT_DESKTOP, g.options.desktopShortcut ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        case WM_NOTIFY:
            switch (reinterpret_cast<NMHDR*>(lParam)->code)
            {
            case PSN_SETACTIVE:
                SetButtons(dialog, PSWIZB_BACK | PSWIZB_NEXT);
                return TRUE;
            case PSN_WIZNEXT:
                g.options.autoUpdate = IsDlgButtonChecked(dialog, IDC_OPT_UPDATES) == BST_CHECKED;
                g.options.desktopShortcut = IsDlgButtonChecked(dialog, IDC_OPT_DESKTOP) == BST_CHECKED;
                return TRUE;
            }
            return FALSE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK ReadyPage(HWND dialog, UINT message, WPARAM, LPARAM lParam)
    {
        if (message == WM_APP_NEXT_TEXT)
        {
            SetNextLabel(dialog, g.updateMode ? L"&Update" : L"&Install");
            return TRUE;
        }
        if (message != WM_NOTIFY)
            return FALSE;
        switch (reinterpret_cast<NMHDR*>(lParam)->code)
        {
        case PSN_SETACTIVE:
        {
            std::wstring text;
            if (g.updateMode)
            {
                text = L"Update the installation in:\r\n    " + g.options.root.wstring() + L"\r\n\r\n";
                text += L"From version " HC09_VERSION_W L" to " + (g.targetRelease.empty() ? L"the latest release" : g.targetRelease) + L".\r\n\r\n";
                text += L"The new source is downloaded, then the game is recompiled and rebuilt. Your saves (build\\userdata) are kept.";
            }
            else
            {
                text = L"Install folder:\r\n    " + g.options.root.wstring() + L"\r\n\r\n";
                text += L"Disc image:\r\n    " + g.options.iso.wstring() + L"\r\n\r\n";
                text += L"Automatic update checks: " + std::wstring(g.options.autoUpdate ? L"on" : L"off") + L"\r\n";
                text += L"Desktop shortcut: " + std::wstring(g.options.desktopShortcut ? L"yes" : L"no") + L"\r\n\r\n";
                text += L"Setup will download the project and its build tools (XenonRecomp, FFmpeg), import the game's "
                        L"executable from the disc image, recompile it and build HC09.exe.";
            }
            SetDlgItemTextW(dialog, IDC_READY_TEXT, text.c_str());
            SetButtons(dialog, (g.updateMode ? 0 : PSWIZB_BACK) | PSWIZB_NEXT);
            // The sheet resets the button labels while activating a page.
            PostMessageW(dialog, WM_APP_NEXT_TEXT, 0, 0);
            return TRUE;
        }
        case PSN_WIZBACK:
            SetNextLabel(dialog, L"&Next >");
            return TRUE;
        }
        return FALSE;
    }

    void AppendLog(HWND dialog, const std::wstring& line)
    {
        HWND log = GetDlgItem(dialog, IDC_PROG_LOG);
        // Keep the control responsive: drop the oldest text past ~1 MB.
        if (GetWindowTextLengthW(log) > 1000000)
        {
            SendMessageW(log, EM_SETSEL, 0, 200000);
            SendMessageW(log, EM_REPLACESEL, FALSE, LPARAM(L""));
        }
        const int end = GetWindowTextLengthW(log);
        SendMessageW(log, EM_SETSEL, end, end);
        const std::wstring text = line + L"\r\n";
        SendMessageW(log, EM_REPLACESEL, FALSE, LPARAM(text.c_str()));
    }

    void StartPipeline(HWND dialog)
    {
        SetDlgItemTextW(dialog, IDC_PROG_LOG, L"");
        SendDlgItemMessageW(dialog, IDC_PROG_BAR, PBM_SETPOS, 0, 0);
        PipelineCallbacks callbacks;
        callbacks.step = [dialog](int step, int steps, const std::wstring& title) {
            SendDlgItemMessageW(dialog, IDC_PROG_BAR, PBM_SETRANGE32, 0, steps);
            PostMessageW(dialog, WM_APP_STEP, WPARAM(step), LPARAM(new std::wstring(title)));
        };
        callbacks.log = [dialog](const std::wstring& line) { PostMessageW(dialog, WM_APP_LOG, 0, LPARAM(new std::wstring(line))); };
        g.pipeline = std::make_unique<Pipeline>(g.toolchain, g.options, std::move(callbacks));
        g.running = true;
        g.succeeded = false;
        g.worker = std::thread([dialog] {
            const bool ok = g.pipeline->Run();
            PostMessageW(dialog, WM_APP_DONE, ok, 0);
        });
    }

    INT_PTR CALLBACK ProgressPage(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_INITDIALOG:
        {
            // A fixed-pitch font makes build output readable.
            HDC dc = GetDC(dialog);
            const int height = -MulDiv(8, GetDeviceCaps(dc, LOGPIXELSY), 72);
            ReleaseDC(dialog, dc);
            HFONT font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
            SendDlgItemMessageW(dialog, IDC_PROG_LOG, WM_SETFONT, WPARAM(font), FALSE);
            SendDlgItemMessageW(dialog, IDC_PROG_LOG, EM_SETLIMITTEXT, 0, 0);
            return TRUE;
        }
        case WM_APP_STEP:
        {
            std::unique_ptr<std::wstring> title(reinterpret_cast<std::wstring*>(lParam));
            SetDlgItemTextW(dialog, IDC_PROG_STEP, title->c_str());
            SendDlgItemMessageW(dialog, IDC_PROG_BAR, PBM_SETPOS, wParam, 0);
            return TRUE;
        }
        case WM_APP_LOG:
        {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lParam));
            AppendLog(dialog, *line);
            return TRUE;
        }
        case WM_APP_DONE:
            if (g.worker.joinable())
                g.worker.join();
            g.running = false;
            g.succeeded = wParam != 0;
            SetDlgItemTextW(dialog, IDC_PROG_STEP, g.succeeded ? (g.updateMode ? L"The update is complete." : L"Installation is complete.")
                : L"Setup did not finish. The log below says why; click Back to change your choices, or Next to try again.");
            if (g.succeeded)
                SetNextLabel(dialog, L"&Next >");
            SetButtons(dialog, g.succeeded ? PSWIZB_NEXT : (PSWIZB_BACK | PSWIZB_NEXT));
            return TRUE;
        case WM_NOTIFY:
            switch (reinterpret_cast<NMHDR*>(lParam)->code)
            {
            case PSN_SETACTIVE:
                SetButtons(dialog, 0);
                StartPipeline(dialog);
                return TRUE;
            case PSN_WIZNEXT:
                if (!g.succeeded)
                {
                    // Try again after a failure.
                    SetButtons(dialog, 0);
                    SetNextLabel(dialog, g.updateMode ? L"&Update" : L"&Install");
                    StartPipeline(dialog);
                    SetResult(dialog, -1);
                }
                return TRUE;
            case PSN_WIZBACK:
                SetNextLabel(dialog, L"&Next >");
                return TRUE;
            case PSN_QUERYCANCEL:
                if (g.running)
                {
                    if (MessageBoxW(GetParent(dialog), L"Stop setup? The parts already downloaded or built are kept for the next attempt.",
                            kProduct, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    {
                        SetResult(dialog, TRUE);
                        return TRUE;
                    }
                    g.pipeline->Cancel();
                    if (g.worker.joinable())
                        g.worker.join();
                    g.running = false;
                }
                SetResult(dialog, FALSE);
                return TRUE;
            }
            return FALSE;
        }
        return FALSE;
    }

    INT_PTR CALLBACK FinishPage(HWND dialog, UINT message, WPARAM, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            ApplyTitleFont(dialog);
            SetDlgItemTextW(dialog, IDC_TITLE, g.updateMode ? L"The NFL Head Coach 09 Recompilation has been updated"
                                                            : L"Completing the NFL Head Coach 09 Recompilation Setup Wizard");
            CheckDlgButton(dialog, IDC_FIN_LAUNCH, BST_CHECKED);
            return TRUE;
        }
        if (message != WM_NOTIFY)
            return FALSE;
        switch (reinterpret_cast<NMHDR*>(lParam)->code)
        {
        case PSN_SETACTIVE:
        {
            std::wstring text = L"The game was built in:\n" + g.options.root.wstring() + L"\\build\\hc09\\HC09.exe\n\n";
            if (!g.updateMode && std::filesystem::exists(KnownFolder(FOLDERID_Programs) / L"NFL Head Coach 09 (Recompiled).lnk"))
                text += L"A Start menu shortcut named \"NFL Head Coach 09 (Recompiled)\" was created.\n\n";
            text += L"Saves are kept in build\\userdata. To change the disc image later, run setup again.";
            SetDlgItemTextW(dialog, IDC_TEXT, text.c_str());
            SetButtons(dialog, PSWIZB_FINISH);
            EnableWindow(GetDlgItem(GetParent(dialog), IDCANCEL), FALSE);  // nothing left to cancel
            return TRUE;
        }
        case PSN_WIZFINISH:
            if (IsDlgButtonChecked(dialog, IDC_FIN_LAUNCH) == BST_CHECKED)
            {
                const std::filesystem::path game = g.options.root / L"build\\hc09\\HC09.exe";
                ShellExecuteW(nullptr, L"open", game.c_str(), nullptr, g.options.root.c_str(), SW_SHOWNORMAL);
            }
            return TRUE;
        }
        return FALSE;
    }

    PROPSHEETPAGEW Page(int dialog, DLGPROC procedure, const wchar_t* title = nullptr, const wchar_t* subtitle = nullptr)
    {
        PROPSHEETPAGEW page{ sizeof(page) };
        page.hInstance = g.instance;
        page.pszTemplate = MAKEINTRESOURCEW(dialog);
        page.pfnDlgProc = procedure;
        // A wizard's title bar shows the page's title: the product name on every page.
        page.dwFlags = PSP_USETITLE;
        page.pszTitle = g.updateMode ? L"NFL Head Coach 09 Recompilation Update" : L"NFL Head Coach 09 Recompilation Setup";
        if (title)
        {
            page.dwFlags |= PSP_USEHEADERTITLE | PSP_USEHEADERSUBTITLE;
            page.pszHeaderTitle = title;
            page.pszHeaderSubTitle = subtitle;
        }
        else
        {
            page.dwFlags |= PSP_HIDEHEADER;
        }
        return page;
    }

    // The default install folder: the project this setup was built in, else %USERPROFILE%\HC09Recomp.
    std::filesystem::path DefaultRoot()
    {
        for (std::filesystem::path folder = ExecutablePath().parent_path(); !folder.empty() && folder != folder.root_path();
             folder = folder.parent_path())
            if (IsProjectFolder(folder))
                return folder;
        return KnownFolder(FOLDERID_Profile) / L"HC09Recomp";
    }

    // The updater rebuilds build\installer\HC09Setup.exe, so it runs from a temporary copy.
    bool RelaunchFromTemp(const std::wstring& commandLine)
    {
        const std::filesystem::path self = ExecutablePath();
        std::wstring selfText = self.wstring(), rootText = g.options.root.wstring();
        if (rootText.empty() || _wcsnicmp(selfText.c_str(), rootText.c_str(), rootText.size()) != 0)
            return false;
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::filesystem::path copy = std::filesystem::path(temp) / L"HC09Setup-update.exe";
        if (!CopyFileW(self.c_str(), copy.c_str(), FALSE))
            return false;
        std::wstring line = L"\"" + copy.wstring() + L"\" " + commandLine;
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
            return false;
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return true;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    g.instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++)
    {
        // HC09Setup.exe --import <iso> <project folder>: only the disc import step (no window), for
        // developers following the manual pipeline. Exit code 0 on success.
        if (_wcsicmp(argv[i], L"--import") == 0 && i + 2 < argc)
        {
            std::wstring error, summary;
            const bool ok = ImportGameFiles(argv[i + 1], argv[i + 2], error, summary);
            if (AttachConsole(ATTACH_PARENT_PROCESS))
            {
                const std::wstring text = (ok ? summary : L"error: " + error) + L"\n";
                DWORD written = 0;
                WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), text.c_str(), DWORD(text.size()), &written, nullptr);
            }
            LocalFree(argv);
            return ok ? 0 : 1;
        }
        if (_wcsicmp(argv[i], L"--update") == 0 && i + 1 < argc)
        {
            g.updateMode = true;
            g.options.update = true;
            g.options.root = std::filesystem::path(argv[++i]).lexically_normal();
        }
    }
    if (g.updateMode)
    {
        if (RelaunchFromTemp(L"--update \"" + g.options.root.wstring() + L"\""))
            return 0;
        // The update keeps the disc image and update preference from hc09.ini.
        const Settings settings = LoadSettings(g.options.root);
        g.options.iso = settings.isoPath;
        g.options.autoUpdate = settings.autoUpdate;
    }
    else
    {
        g.options.root = DefaultRoot();
    }
    LocalFree(argv);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);
    g.requirements = CheckRequirements(g.toolchain);
    if (g.updateMode)
        if (const auto tag = LatestReleaseTag(8000))
            g.targetRelease = std::wstring(tag->begin(), tag->end());

    std::vector<PROPSHEETPAGEW> pages;
    if (!g.updateMode)
        pages.push_back(Page(IDD_WELCOME, WelcomePage));
    // Updating with the toolchain still in place: nothing to show on this page.
    if (!g.updateMode || !RequirementsMet())
        pages.push_back(Page(IDD_REQUIREMENTS, RequirementsPage, L"Requirements", L"Setup checks that this PC can build the game."));
    if (!g.updateMode)
    {
        pages.push_back(Page(IDD_LOCATION, LocationPage, L"Install Location", L"Choose where the game is installed and built."));
        pages.push_back(Page(IDD_ISO, IsoPage, L"Game Disc Image", L"Select your NFL Head Coach 09 disc image to import the game from."));
        pages.push_back(Page(IDD_OPTIONS, OptionsPage, L"Options", L"Choose how the game is kept up to date."));
    }
    pages.push_back(Page(IDD_READY, ReadyPage, g.updateMode ? L"Ready to Update" : L"Ready to Install",
        L"Review your choices, then start."));
    pages.push_back(Page(IDD_PROGRESS, ProgressPage, g.updateMode ? L"Updating" : L"Installing",
        L"Please wait while the game is downloaded, imported and built."));
    pages.push_back(Page(IDD_FINISH, FinishPage));

    HBITMAP watermark = CreateArtwork(164, 314, true);
    HBITMAP header = CreateArtwork(49, 49, false);
    PROPSHEETHEADERW sheet{ sizeof(sheet) };
    sheet.dwFlags = PSH_WIZARD97 | PSH_WATERMARK | PSH_HEADER | PSH_USEHBMWATERMARK | PSH_USEHBMHEADER | PSH_PROPSHEETPAGE | PSH_USEICONID;
    sheet.hInstance = instance;
    sheet.pszIcon = MAKEINTRESOURCEW(IDI_SETUP);
    sheet.pszCaption = g.updateMode ? L"NFL Head Coach 09 Recompilation Update" : L"NFL Head Coach 09 Recompilation Setup";
    sheet.nPages = UINT(pages.size());
    sheet.ppsp = pages.data();
    sheet.hbmWatermark = watermark;
    sheet.hbmHeader = header;
    PropertySheetW(&sheet);

    if (g.worker.joinable())
    {
        g.pipeline->Cancel();
        g.worker.join();
    }
    DeleteObject(watermark);
    DeleteObject(header);
    if (g.titleFont)
        DeleteObject(g.titleFont);
    CoUninitialize();
    return 0;
}
