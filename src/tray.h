#pragma once
#include <windows.h>

#include <shellapi.h>

#include <string>
#include <vector>

class Tray {
public:
    bool Create(HWND hwnd, UINT callbackMsg, HICON icon);
    void Recreate();  // after Explorer restarts (TaskbarCreated)
    void Destroy();
    void SetTip(const std::wstring& tip);
    void Balloon(const std::wstring& title, const std::wstring& text, bool error);

private:
    bool Add();
    NOTIFYICONDATAW nid_{};
    bool added_ = false;
    std::wstring tip_;
};

struct TrayMenuModel {
    struct Monitor {
        std::wstring label;
        std::vector<std::wstring> shaders;  // file names in shaders/
        int selected = -1;
    };
    std::vector<Monitor> monitors;
    bool paused = false;
    int forcedFps = 0;
    bool showStats = false;
    bool autostart = false;
    int theme = 0;  // 0 = follow the taskbar, 1 = light, 2 = dark
};

enum TrayCommand : UINT {
    kCmdNone = 0,
    kCmdPauseToggle = 100,
    kCmdFpsAuto = 110,
    kCmdFps120,
    kCmdFps60,
    kCmdFps30,
    kCmdFps15,
    kCmdOpenConfig = 120,
    kCmdOpenShaders,
    kCmdReload,
    kCmdShowStats,
    kCmdAutostart,
    kCmdExit,
    kCmdThemeSystem = 130,
    kCmdThemeLight,
    kCmdThemeDark,
    kCmdShaderBase = 1000,  // + monitor * 256 + shader index
};

// Shows the tray menu at `pt` and returns the chosen command (kCmdNone if dismissed).
UINT ShowTrayMenu(HWND hwnd, const TrayMenuModel& model, POINT pt);
