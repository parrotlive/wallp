#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "desktop.h"

// ---- fullscreen / covered ----

struct WindowScan {
    std::vector<bool> fullscreen;  // the foreground app is fullscreen (or exclusive-mode) on this monitor
    std::vector<bool> covered;     // no part of this monitor's work area is visible
};

// One pass over the top-level windows. Cloaked, minimized, tool and transparent overlay
// windows do not count as covering.
WindowScan ScanWindows(const std::vector<MonitorInfo>& monitors);

// ---- session / power ----

struct SessionState {
    bool locked = false;
    bool disconnected = false;  // fast user switching / RDP disconnect
    bool displayOff = false;
    bool suspended = false;
    bool onBattery = false;
};

class SessionWatcher {
public:
    void Register(HWND hwnd);
    void Unregister();
    // Returns true if the message was a session or power notification (state updated).
    bool HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    const SessionState& State() const { return state_; }

private:
    HWND hwnd_ = nullptr;
    HPOWERNOTIFY display_ = nullptr;
    HPOWERNOTIFY acdc_ = nullptr;
    SessionState state_;
};

uint32_t UserIdleMs();

// SetWinEventHook for window changes that can uncover or cover the desktop. The callback
// runs on the installing thread (out-of-context hook).
class WindowEventHook {
public:
    using Callback = void (*)(void* ctx);
    bool Install(Callback cb, void* ctx);
    void Uninstall();
    static uint32_t TakeEventCount();  // raw events delivered since the last call

private:
    static void CALLBACK Proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
    std::vector<HWINEVENTHOOK> hooks_;
};

// ---- GPU usage per process (PDH "GPU Engine" counters, 3D engines) ----

struct GpuActivity {
    bool valid = false;
    bool game = false;  // a listed game, or an unlisted process above the threshold
    bool busy = false;  // a "video" process above the threshold
    std::wstring top;   // busiest process, for logs and stats
    double topLoad = 0.0;
    double selfLoad = 0.0;  // wallp's own 3D engine load
};

class GpuMonitor {
public:
    ~GpuMonitor() { Close(); }
    // Collects a sample. The first sample after Unprime() only primes the rate counters.
    GpuActivity Sample(const std::vector<std::wstring>& games, const std::vector<std::wstring>& ignore,
                       const std::vector<std::wstring>& video, double threshold);
    void Unprime() { primed_ = false; }
    void Close();

private:
    bool Open();
    const std::wstring& NameOf(DWORD pid);

    void* query_ = nullptr;
    void* counter_ = nullptr;
    bool primed_ = false;
    bool failed_ = false;
    std::unordered_map<DWORD, std::wstring> names_;
    std::vector<BYTE> buffer_;
};

// ---- CPU / memory ----

class CpuLoad {
public:
    float Sample();  // whole system, 0..1, since the previous call
private:
    uint64_t idle_ = 0, total_ = 0;
};

class ProcessUsage {
public:
    // CPU in percent of the whole machine (like Task Manager) since the previous call, and
    // the private working set in MB.
    void Sample(double& cpuPercent, double& memoryMB);

private:
    uint64_t cycles_ = 0, tsc_ = 0;  // cycle-based: exact even for short wake-ups
    uint64_t cpu_ = 0;               // tick-based fallback (100 ns units)
    int64_t qpc_ = 0;
};
