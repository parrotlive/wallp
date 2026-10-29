#pragma once
#include <windows.h>

#include <string>
#include <vector>

struct MonitorInfo {
    int index = 0;           // 0 = primary, then left-to-right / top-to-bottom
    HMONITOR handle = nullptr;
    RECT rect{};             // virtual-desktop coordinates (physical pixels)
    RECT work{};             // work area (rect minus taskbar)
    std::wstring device;     // e.g. \\.\DISPLAY1
    int hz = 60;
    bool primary = false;
};

// Where wallpaper windows get parented. All of this is undocumented Explorer behavior and is
// kept in this file.
struct DesktopHost {
    HWND parent = nullptr;       // our windows become children of this
    HWND insertAfter = nullptr;  // if set, place our windows directly below this sibling
    HWND progman = nullptr;
    HWND defView = nullptr;      // SHELLDLL_DefView (the icon layer)
    const char* layout = "none";
};

std::vector<MonitorInfo> EnumMonitors();
bool SameMonitorLayout(const std::vector<MonitorInfo>& a, const std::vector<MonitorInfo>& b);

// Spawns (if needed) and finds the WorkerW behind the desktop icons.
bool FindDesktopHost(DesktopHost& host);
// Shell windows that are part of the desktop itself (never "cover" it, never "fullscreen").
bool IsShellDesktopWindow(HWND hwnd);
// Makes Explorer repaint the regular wallpaper (used on exit).
void RefreshDesktopWallpaper();
