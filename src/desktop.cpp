#include "desktop.h"

#include <algorithm>

#include "util.h"

// ---- monitors ----

static BOOL CALLBACK MonitorEnumProc(HMONITOR hm, HDC, LPRECT, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<MonitorInfo>*>(lp);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    MonitorInfo m;
    m.handle = hm;
    m.rect = mi.rcMonitor;
    m.work = mi.rcWork;
    m.device = mi.szDevice;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        m.hz = int(dm.dmDisplayFrequency);
    list->push_back(std::move(m));
    return TRUE;
}

std::vector<MonitorInfo> EnumMonitors() {
    std::vector<MonitorInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&list));
    std::sort(list.begin(), list.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    for (size_t i = 0; i < list.size(); ++i) list[i].index = int(i);
    return list;
}

bool SameMonitorLayout(const std::vector<MonitorInfo>& a, const std::vector<MonitorInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!EqualRect(&a[i].rect, &b[i].rect) || a[i].hz != b[i].hz || a[i].device != b[i].device) return false;
    }
    return true;
}

// ---- desktop host (WorkerW) ----

static HWND FindDefViewHost() {
    HWND found = nullptr;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            if (FindWindowExW(h, nullptr, L"SHELLDLL_DefView", nullptr)) {
                *reinterpret_cast<HWND*>(lp) = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found;
}

// Returns true if a WorkerW was found; on false `host` may still hold the Progman fallback.
static bool Locate(DesktopHost& host) {
    host.parent = host.insertAfter = host.defView = nullptr;
    host.layout = "none";
    HWND defHost = FindDefViewHost();
    if (!defHost) return false;
    host.defView = FindWindowExW(defHost, nullptr, L"SHELLDLL_DefView", nullptr);

    if (defHost != host.progman) {
        // Classic layout (Windows 10, Windows 11 up to 23H2): the icons live in a top-level
        // WorkerW and the wallpaper goes into the next top-level WorkerW in z-order.
        if (HWND w = FindWindowExW(nullptr, defHost, L"WorkerW", nullptr)) {
            host.parent = w;
            host.layout = "classic WorkerW";
            return true;
        }
        host.parent = host.progman;  // last resort: Progman sits below the icon WorkerW
        host.layout = "classic Progman";
        return false;
    }

    // Windows 11 24H2+: SHELLDLL_DefView and the wallpaper WorkerW are both children of
    // Progman, with the WorkerW below the icons.
    if (HWND w = FindWindowExW(host.progman, nullptr, L"WorkerW", nullptr)) {
        host.parent = w;
        host.layout = "Progman child WorkerW";
        return true;
    }
    host.parent = host.progman;  // last resort: a Progman child just below the icon layer
    host.insertAfter = host.defView;
    host.layout = "Progman";
    return false;
}

bool FindDesktopHost(DesktopHost& host) {
    host = {};
    host.progman = FindWindowW(L"Progman", nullptr);
    if (!host.progman) return false;

    // 0x052C asks Progman to create the WorkerW behind the icons (no-op if it exists).
    DWORD_PTR result = 0;
    SendMessageTimeoutW(host.progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
    if (Locate(host)) return true;
    SendMessageTimeoutW(host.progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    Locate(host);
    return host.parent != nullptr;
}

bool IsShellDesktopWindow(HWND hwnd) {
    wchar_t cls[64];
    if (GetClassNameW(hwnd, cls, 64) <= 0) return false;
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

void RefreshDesktopWallpaper() {
    // Re-applying the current wallpaper (without saving it) makes Explorer repaint the desktop.
    wchar_t path[1024] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, ARRAYSIZE(path), path, 0))
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
}
