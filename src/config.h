#pragma once
#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

struct Config {
    std::map<int, std::wstring> monitors;  // monitor index -> shader path (relative to the exe dir, or absolute)
    int fpsIdle = 120;
    int fpsBusy = 60;
    int fpsGame = 15;
    int fpsBattery = 30;
    float renderScale = 1.0f;
    int idleTimeoutSec = 600;
    float gpuGameThreshold = 0.20f;
    std::vector<std::wstring> games;   // lowercase exe names, always treated as games
    std::vector<std::wstring> ignore;  // never treated as games / GPU-busy
    std::vector<std::wstring> video;   // GPU-heavy but not games -> "busy" tier
    bool autostart = false;
    bool showStats = true;
    int theme = 0;                     // tray menu: 0 = follow the taskbar, 1 = light, 2 = dark
    int releaseAfterPauseSec = 300;    // free swapchain buffers after this long paused (0 = never)
};

Config DefaultConfig();
// Parses config.json text on top of the defaults. On failure `cfg` is untouched and `error` is set.
bool ParseConfig(std::string text, Config& cfg, std::string& error);
bool LoadConfig(const std::wstring& path, Config& cfg, std::string& error);
bool WriteDefaultConfig(const std::wstring& path);
// Writes the tray-editable settings (monitors, autostart, showStats, theme) into config.json and
// leaves every other key as it is. Comments in the file are not preserved.
bool SaveConfig(const std::wstring& path, const Config& cfg, std::string& error);

// Autostart through HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
bool SetAutostart(bool enable);
bool IsAutostartEnabled();

// Overlapped ReadDirectoryChangesW on a few directories. The owner waits on Event(i) and
// calls Collect(i) when it is signaled.
class DirWatcher {
public:
    struct Change {
        int tag;
        std::wstring path;  // full path of the changed entry; empty if the change list overflowed
    };

    DirWatcher() = default;
    ~DirWatcher() { Clear(); }
    DirWatcher(const DirWatcher&) = delete;
    DirWatcher& operator=(const DirWatcher&) = delete;

    bool Watch(const std::wstring& dir, bool subtree, int tag);
    void Clear();
    size_t Count() const { return entries_.size(); }
    HANDLE Event(size_t i) const { return entries_[i]->ov.hEvent; }
    void Collect(size_t i, std::vector<Change>& out);

private:
    struct Entry {
        std::wstring dir;
        int tag = 0;
        bool subtree = false;
        bool pending = false;
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED ov{};
        alignas(8) BYTE buffer[16384];
    };
    static bool Arm(Entry& e);
    std::vector<std::unique_ptr<Entry>> entries_;
};
