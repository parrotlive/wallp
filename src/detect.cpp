#include "detect.h"

#include <dwmapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wtsapi32.h>

#include <algorithm>
#include <intrin.h>

#include "util.h"

// winnt.h declares these with DEFINE_GUID; keep local definitions instead of linking a lib.
static const GUID kConsoleDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
static const GUID kAcDcPowerSource = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};

// ---- fullscreen / covered ----

static bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

static bool Contains(const RECT& outer, const RECT& inner) {
    return outer.left <= inner.left && outer.top <= inner.top && outer.right >= inner.right &&
           outer.bottom >= inner.bottom;
}

// The shell's view (exclusive-mode D3D app, presentation mode). It costs ~70 us to query,
// more than the rest of a window scan together, and rarely changes: refresh it every 2 s.
static QUERY_USER_NOTIFICATION_STATE ShellNotificationState() {
    static QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    static ULONGLONG queried = 0;
    ULONGLONG now = GetTickCount64();
    if (queried == 0 || now - queried >= 2000) {
        if (FAILED(SHQueryUserNotificationState(&state))) state = QUNS_ACCEPTS_NOTIFICATIONS;
        queried = now;
    }
    return state;
}

// Click-through and translucent overlays (game/driver overlays and the like) do not hide the desktop.
static bool IsOpaque(HWND hwnd, LONG exStyle) {
    if (exStyle & WS_EX_TRANSPARENT) return false;
    if (exStyle & WS_EX_LAYERED) {
        COLORREF key = 0;
        BYTE alpha = 255;
        DWORD flags = 0;
        if (!GetLayeredWindowAttributes(hwnd, &key, &alpha, &flags)) return false;  // per-pixel alpha
        if ((flags & LWA_COLORKEY) || ((flags & LWA_ALPHA) && alpha < 255)) return false;
    }
    return true;
}

namespace {
struct ScanContext {
    const std::vector<MonitorInfo>* monitors;
    std::vector<HRGN> visible;  // the still-visible part of each work area
    std::vector<bool> done;
    size_t remaining;
};
}  // namespace

static BOOL CALLBACK ScanProc(HWND hwnd, LPARAM lp) {
    auto& c = *reinterpret_cast<ScanContext*>(lp);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) || !IsOpaque(hwnd, ex)) return TRUE;
    RECT r;
    if (!GetWindowRect(hwnd, &r) || IsRectEmpty(&r)) return TRUE;

    const auto& mons = *c.monitors;
    bool relevant = false;
    for (size_t i = 0; i < mons.size() && !relevant; ++i) {
        RECT tmp;
        relevant = !c.done[i] && IntersectRect(&tmp, &r, &mons[i].work);
    }
    if (!relevant || IsCloaked(hwnd) || IsShellDesktopWindow(hwnd)) return TRUE;

    // The visible frame, without the invisible resize borders.
    RECT frame;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame)))) r = frame;
    HRGN win = CreateRectRgnIndirect(&r);
    for (size_t i = 0; i < mons.size(); ++i) {
        RECT tmp;
        if (c.done[i] || !IntersectRect(&tmp, &r, &mons[i].work)) continue;
        if (CombineRgn(c.visible[i], c.visible[i], win, RGN_DIFF) == NULLREGION) {
            c.done[i] = true;
            --c.remaining;
        }
    }
    DeleteObject(win);
    return c.remaining > 0;
}

WindowScan ScanWindows(const std::vector<MonitorInfo>& monitors) {
    WindowScan s;
    s.fullscreen.assign(monitors.size(), false);
    s.covered.assign(monitors.size(), false);
    if (monitors.empty()) return s;

    // Fullscreen: the foreground window fills its monitor, or the shell reports an
    // exclusive-mode D3D app.
    HWND fg = GetForegroundWindow();
    if (fg && IsWindowVisible(fg) && !IsIconic(fg) && !IsShellDesktopWindow(fg) && !IsCloaked(fg)) {
        HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONULL);
        RECT r{};
        GetWindowRect(fg, &r);
        for (size_t i = 0; i < monitors.size(); ++i) {
            if (monitors[i].handle != hm) continue;
            if (Contains(r, monitors[i].rect)) {
                s.fullscreen[i] = true;
                continue;
            }
            QUERY_USER_NOTIFICATION_STATE quns = ShellNotificationState();
            s.fullscreen[i] = quns == QUNS_RUNNING_D3D_FULL_SCREEN || (quns == QUNS_BUSY && Contains(r, monitors[i].work));
        }
    }

    // Covered: subtract every qualifying window from each work area (the taskbar strip is
    // not part of the work area, so a maximized window counts as covering).
    ScanContext c{&monitors, {}, std::vector<bool>(monitors.size(), false), monitors.size()};
    c.visible.reserve(monitors.size());
    for (const auto& m : monitors) c.visible.push_back(CreateRectRgnIndirect(&m.work));
    EnumWindows(ScanProc, reinterpret_cast<LPARAM>(&c));
    for (size_t i = 0; i < monitors.size(); ++i) {
        s.covered[i] = c.done[i];
        DeleteObject(c.visible[i]);
    }
    return s;
}

// ---- session / power ----

void SessionWatcher::Register(HWND hwnd) {
    hwnd_ = hwnd;
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    // Both registrations also deliver the current value right away.
    display_ = RegisterPowerSettingNotification(hwnd, &kConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    acdc_ = RegisterPowerSettingNotification(hwnd, &kAcDcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) state_.onBattery = ps.ACLineStatus == 0;
}

void SessionWatcher::Unregister() {
    if (!hwnd_) return;
    WTSUnRegisterSessionNotification(hwnd_);
    if (display_) UnregisterPowerSettingNotification(display_);
    if (acdc_) UnregisterPowerSettingNotification(acdc_);
    display_ = acdc_ = nullptr;
    hwnd_ = nullptr;
}

bool SessionWatcher::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WTSSESSION_CHANGE) {
        switch (wp) {
        case WTS_SESSION_LOCK: state_.locked = true; break;
        case WTS_SESSION_UNLOCK: state_.locked = false; break;
        case WTS_CONSOLE_DISCONNECT:
        case WTS_REMOTE_DISCONNECT: state_.disconnected = true; break;
        case WTS_CONSOLE_CONNECT:
        case WTS_REMOTE_CONNECT: state_.disconnected = false; break;
        }
        return true;
    }
    if (msg != WM_POWERBROADCAST) return false;
    switch (wp) {
    case PBT_APMSUSPEND: state_.suspended = true; break;
    case PBT_APMRESUMEAUTOMATIC:
    case PBT_APMRESUMESUSPEND: state_.suspended = false; break;
    case PBT_APMPOWERSTATUSCHANGE: {
        SYSTEM_POWER_STATUS ps;
        if (GetSystemPowerStatus(&ps)) state_.onBattery = ps.ACLineStatus == 0;
        break;
    }
    case PBT_POWERSETTINGCHANGE: {
        auto* s = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
        if (!s || s->DataLength < sizeof(DWORD)) break;
        DWORD value = *reinterpret_cast<const DWORD*>(s->Data);
        if (IsEqualGUID(s->PowerSetting, kConsoleDisplayState))
            state_.displayOff = value == 0;  // 0 off, 1 on, 2 dimmed
        else if (IsEqualGUID(s->PowerSetting, kAcDcPowerSource))
            state_.onBattery = value != 0;  // 0 AC, 1 battery, 2 short-term source (UPS)
        break;
    }
    }
    return true;
}

uint32_t UserIdleMs() {
    LASTINPUTINFO li{};
    li.cbSize = sizeof(li);
    if (!GetLastInputInfo(&li)) return 0;
    return GetTickCount() - li.dwTime;
}

// ---- window events ----

static WindowEventHook::Callback g_eventCallback = nullptr;
static void* g_eventContext = nullptr;
static std::atomic<uint32_t> g_eventCount{0};

bool WindowEventHook::Install(Callback cb, void* ctx) {
    g_eventCallback = cb;
    g_eventContext = ctx;
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    auto add = [&](DWORD lo, DWORD hi) {
        if (HWINEVENTHOOK h = SetWinEventHook(lo, hi, nullptr, Proc, 0, 0, flags)) hooks_.push_back(h);
    };
    // Deliberately not EVENT_OBJECT_LOCATIONCHANGE: it also fires for every cursor move, and
    // each out-of-context event wakes this thread (measured: ~125 events/s while the mouse
    // moves, ~32 us each, 0.4% of a core). Maximize, restore and snap, the window changes it
    // would add, are picked up by the 500 ms policy tick instead.
    add(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND);
    add(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND);
    add(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND);
    add(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED);
    return !hooks_.empty();
}

void WindowEventHook::Uninstall() {
    for (HWINEVENTHOOK h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
    g_eventCallback = nullptr;
}

uint32_t WindowEventHook::TakeEventCount() { return g_eventCount.exchange(0); }

void CALLBACK WindowEventHook::Proc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD,
                                    DWORD) {
    g_eventCount.fetch_add(1, std::memory_order_relaxed);
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
    if (event >= EVENT_OBJECT_CLOAKED && (GetWindowLongW(hwnd, GWL_STYLE) & WS_CHILD)) return;  // top-level only
    if (g_eventCallback) g_eventCallback(g_eventContext);
}

// ---- GPU ----

bool GpuMonitor::Open() {
    if (query_) return true;
    if (failed_) return false;
    PDH_HQUERY q = nullptr;
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &q);
    PDH_HCOUNTER c = nullptr;
    if (st == ERROR_SUCCESS)
        st = PdhAddEnglishCounterW(q, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &c);
    if (st != ERROR_SUCCESS) {
        Log("GPU engine counters unavailable (0x%08lX); game detection uses the games list only",
            static_cast<unsigned long>(st));
        if (q) PdhCloseQuery(q);
        failed_ = true;
        return false;
    }
    query_ = q;
    counter_ = c;
    primed_ = false;
    return true;
}

void GpuMonitor::Close() {
    if (query_) PdhCloseQuery(static_cast<PDH_HQUERY>(query_));
    query_ = counter_ = nullptr;
    primed_ = false;
}

const std::wstring& GpuMonitor::NameOf(DWORD pid) {
    auto it = names_.find(pid);
    if (it != names_.end()) return it->second;
    std::wstring name;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[1024];
        DWORD len = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) name = Lower(FileNameOf(std::wstring(buf, len)));
        CloseHandle(h);
    }
    if (name.empty()) {
        // Some system processes (dwm.exe among them) refuse even limited query access; a
        // toolhelp snapshot lists their names without opening them.
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
                if (pe.th32ProcessID == pid) {
                    name = Lower(pe.szExeFile);
                    break;
                }
            }
            CloseHandle(snap);
        }
    }
    return names_.emplace(pid, std::move(name)).first->second;
}

static bool Listed(const std::vector<std::wstring>& list, const std::wstring& name) {
    return !name.empty() && std::find(list.begin(), list.end(), name) != list.end();
}

GpuActivity GpuMonitor::Sample(const std::vector<std::wstring>& games, const std::vector<std::wstring>& ignore,
                               const std::vector<std::wstring>& video, double threshold) {
    GpuActivity a;
    if (!Open()) return a;
    if (PdhCollectQueryData(static_cast<PDH_HQUERY>(query_)) != ERROR_SUCCESS) return a;
    if (!primed_) {
        primed_ = true;  // utilization is a rate: it needs two samples
        return a;
    }
    DWORD size = 0, count = 0;
    auto counter = static_cast<PDH_HCOUNTER>(counter_);
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (st != PDH_MORE_DATA) return a;
    buffer_.resize(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
    if (st != ERROR_SUCCESS) return a;

    // Instance names look like "pid_1234_luid_0x..._phys_0_eng_0_engtype_3D".
    std::unordered_map<DWORD, double> load;
    for (DWORD i = 0; i < count; ++i) {
        const auto& v = items[i].FmtValue;
        if (v.CStatus != PDH_CSTATUS_VALID_DATA && v.CStatus != PDH_CSTATUS_NEW_DATA) continue;
        const wchar_t* name = items[i].szName;
        if (!name || wcsncmp(name, L"pid_", 4) != 0) continue;
        load[DWORD(wcstoul(name + 4, nullptr, 10))] += v.doubleValue / 100.0;
    }
    a.valid = true;

    const DWORD self = GetCurrentProcessId();
    for (const auto& [pid, l] : load) {
        if (pid == self) a.selfLoad = l;
        if (pid == self || pid == 0) continue;
        bool heavy = l > threshold;
        if (!heavy && games.empty()) continue;  // only look up names that can matter
        const std::wstring& exe = NameOf(pid);
        if (exe.empty() || exe == L"dwm.exe" || Listed(ignore, exe)) continue;  // empty: already gone
        if (Listed(games, exe)) {
            a.game = true;
        } else if (heavy) {
            if (Listed(video, exe))
                a.busy = true;
            else
                a.game = true;
        }
        if (l > a.topLoad) {
            a.topLoad = l;
            a.top = exe;
        }
    }
    // Forget processes that have no GPU instances any more (pids get reused).
    for (auto it = names_.begin(); it != names_.end();) {
        if (load.count(it->first))
            ++it;
        else
            it = names_.erase(it);
    }
    return a;
}

// ---- CPU / memory ----

static uint64_t ToU64(FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

float CpuLoad::Sample() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0f;
    uint64_t i = ToU64(idle), t = ToU64(kernel) + ToU64(user);  // kernel time includes idle
    float load = 0.0f;
    if (total_ && t > total_) load = 1.0f - float(double(i - idle_) / double(t - total_));
    idle_ = i;
    total_ = t;
    return std::clamp(load, 0.0f, 1.0f);
}

namespace {
// PROCESS_MEMORY_COUNTERS_EX2 (newer SDKs); has the private working set Task Manager shows.
struct MemoryCountersEx2 {
    DWORD cb;
    DWORD PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
    SIZE_T PrivateWorkingSetSize;
    ULONG64 SharedCommitUsage;
};
}  // namespace

void ProcessUsage::Sample(double& cpuPercent, double& memoryMB) {
    // GetProcessTimes charges whole clock ticks to whichever thread is running when the tick
    // fires, which is very noisy for a process that wakes briefly; thread cycle counts are
    // exact and tick at the TSC rate.
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const double cores = double(si.dwNumberOfProcessors);
    cpuPercent = 0.0;
#if defined(_M_X64) || defined(_M_IX86)
    ULONG64 cycles = 0;
    if (QueryProcessCycleTime(GetCurrentProcess(), &cycles)) {
        uint64_t tsc = __rdtsc();
        if (tsc_ && tsc > tsc_) cpuPercent = double(cycles - cycles_) / double(tsc - tsc_) / cores * 100.0;
        cycles_ = cycles;
        tsc_ = tsc;
    } else
#endif
    {
        FILETIME created, exited, kernel, user;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        uint64_t cpu = ToU64(kernel) + ToU64(user);  // 100 ns units
        int64_t now = QpcNow();
        double wall = qpc_ ? QpcToSec(now - qpc_) : 0.0;
        if (wall > 0) cpuPercent = double(cpu - cpu_) * 1e-7 / wall / cores * 100.0;
        cpu_ = cpu;
        qpc_ = now;
    }

    MemoryCountersEx2 m{};
    m.cb = sizeof(m);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)) &&
        m.PrivateWorkingSetSize) {
        memoryMB = double(m.PrivateWorkingSetSize) / (1024.0 * 1024.0);
        return;
    }
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    memoryMB = GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))
                   ? double(pmc.WorkingSetSize) / (1024.0 * 1024.0)
                   : 0.0;
}
