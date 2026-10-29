#include "tray.h"

bool Tray::Create(HWND hwnd, UINT callbackMsg, HICON icon) {
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd;
    nid_.uID = 1;
    nid_.uCallbackMessage = callbackMsg;
    nid_.hIcon = icon;
    wcscpy_s(nid_.szTip, L"wallp");
    tip_ = L"wallp";
    return Add();
}

bool Tray::Add() {
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    added_ = Shell_NotifyIconW(NIM_ADD, &nid_) != 0;
    if (added_) {
        nid_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    }
    return added_;
}

void Tray::Recreate() {
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    Add();
}

void Tray::Destroy() {
    if (added_) Shell_NotifyIconW(NIM_DELETE, &nid_);
    added_ = false;
}

void Tray::SetTip(const std::wstring& tip) {
    if (tip == tip_) return;
    tip_ = tip;
    wcsncpy_s(nid_.szTip, tip.c_str(), _TRUNCATE);
    nid_.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Tray::Balloon(const std::wstring& title, const std::wstring& text, bool error) {
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = error ? NIIF_ERROR : NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Light or dark popup menus. Win32 menus have no documented dark mode; these uxtheme
// exports (by ordinal, Windows 10 1903 and later) are what Explorer itself uses.
static void ApplyMenuTheme(HWND hwnd, int theme) {
    static const bool supported = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
        OSVERSIONINFOW v{sizeof(v)};
        return rtlGetVersion && rtlGetVersion(&v) == 0 && v.dwMajorVersion >= 10 && v.dwBuildNumber >= 18362;
    }();
    if (!supported) return;
    static HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);  // 2 = force dark, 3 = force light
    using AllowDarkModeForWindowFn = bool(WINAPI*)(HWND, bool);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto proc = [](int ordinal) {
        return reinterpret_cast<void*>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(ordinal)));
    };
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(proc(135));
    auto allowWindow = reinterpret_cast<AllowDarkModeForWindowFn>(proc(133));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(proc(136));
    if (!setMode || !flush) return;

    bool dark = theme == 2;
    if (theme == 0) {  // the tray sits on the taskbar, so follow the taskbar's mode
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        dark = light == 0;
    }
    if (allowWindow) allowWindow(hwnd, dark);
    setMode(dark ? 2 : 3);
    flush();
}

UINT ShowTrayMenu(HWND hwnd, const TrayMenuModel& m, POINT pt) {
    ApplyMenuTheme(hwnd, m.theme);
    HMENU menu = CreatePopupMenu();
    for (size_t i = 0; i < m.monitors.size(); ++i) {
        const auto& mon = m.monitors[i];
        HMENU sub = CreatePopupMenu();
        if (mon.shaders.empty()) AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"(no shaders found)");
        for (size_t s = 0; s < mon.shaders.size() && s < 256; ++s) {
            UINT flags = MF_STRING | (int(s) == mon.selected ? MF_CHECKED : 0);
            AppendMenuW(sub, flags, kCmdShaderBase + i * 256 + s, mon.shaders[s].c_str());
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), mon.label.c_str());
    }
    if (!m.monitors.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kCmdPauseToggle, m.paused ? L"Resume" : L"Pause all");
    HMENU fps = CreatePopupMenu();
    AppendMenuW(fps, MF_STRING, kCmdFpsAuto, L"Auto");
    AppendMenuW(fps, MF_STRING, kCmdFps120, L"120");
    AppendMenuW(fps, MF_STRING, kCmdFps60, L"60");
    AppendMenuW(fps, MF_STRING, kCmdFps30, L"30");
    AppendMenuW(fps, MF_STRING, kCmdFps15, L"15");
    UINT checked = m.forcedFps == 120 ? kCmdFps120
                   : m.forcedFps == 60 ? kCmdFps60
                   : m.forcedFps == 30 ? kCmdFps30
                   : m.forcedFps == 15 ? kCmdFps15
                                       : kCmdFpsAuto;
    CheckMenuRadioItem(fps, kCmdFpsAuto, kCmdFps15, checked, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(fps), L"FPS tier");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, kCmdOpenConfig, L"Open config");
    AppendMenuW(menu, MF_STRING, kCmdOpenShaders, L"Open shaders folder");
    AppendMenuW(menu, MF_STRING, kCmdReload, L"Reload");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (m.showStats ? MF_CHECKED : 0), kCmdShowStats, L"Show stats in tooltip");
    AppendMenuW(menu, MF_STRING | (m.autostart ? MF_CHECKED : 0), kCmdAutostart, L"Start with Windows");
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, kCmdThemeSystem, L"System");
    AppendMenuW(theme, MF_STRING, kCmdThemeLight, L"Light");
    AppendMenuW(theme, MF_STRING, kCmdThemeDark, L"Dark");
    CheckMenuRadioItem(theme, kCmdThemeSystem, kCmdThemeDark, kCmdThemeSystem + m.theme, MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"Menu theme");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    UINT cmd = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align,
                                     pt.x, pt.y, hwnd, nullptr));
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenus
    return cmd;
}
