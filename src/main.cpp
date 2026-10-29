// wallp: a performance-first shader wallpaper for Windows 10/11.
//
// Main thread: hidden window + message loop, tray, file watching, session/power events and
// the pause/FPS policy. Render thread (renderer.cpp): the D3D11 device, the wallpaper windows
// and the frame loop. Compile thread (shader.cpp): D3DCompile + bytecode cache.

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "desktop.h"
#include "detect.h"
#include "policy.h"
#include "renderer.h"
#include "resource.h"
#include "shader.h"
#include "tray.h"
#include "util.h"

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_COMPILED = WM_APP + 2;
constexpr UINT WM_APP_RENDER = WM_APP + 3;

enum TimerId : UINT_PTR {
    kTimerPolicy = 1,  // 500 ms re-evaluation (stopped when nothing can resume without an event)
    kTimerScan,        // one-shot: window events and hysteresis rechecks
    kTimerGpu,         // 2 s PDH sample, only while some monitor renders
    kTimerReload,      // one-shot: debounced file changes
    kTimerDisplay,     // one-shot: debounced display / work-area changes
    kTimerReattach,    // one-shot: find the desktop again
    kTimerBench,       // --bench statistics
};

enum WatchTag { kWatchRoot = 1, kWatchShaders = 2 };

struct LoadedShader {
    uint64_t hash = 0;
    std::shared_ptr<const std::vector<uint8_t>> bytecode;
    ShaderSettings settings;
};

struct Slot {  // one per monitor
    MonitorInfo mon;
    std::wstring shaderPath;  // resolved full path
    std::wstring shaderKey;   // lowercase shaderPath
    bool shaderSent = false;
    int shaderMaxFps = 0;
    MonitorDecision decision;
    bool decisionSent = false;
    uint64_t lastFrames = 0, benchFrames = 0;
    double fps = 0.0;
};

std::string FirstLine(const std::string& s) { return s.substr(0, s.find_first_of("\r\n")); }

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void OpenInShell(const std::wstring& path, bool isFile) {
    auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32 && isFile) {
        std::wstring arg = L"\"" + path + L"\"";
        ShellExecuteW(nullptr, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

class App {
public:
    int Run(bool bench);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static void OnWindowEvent(void* self);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void OnTimer(UINT_PTR id);
    bool Init();
    void Shutdown(bool restoreWallpaper);

    void Attach(bool force);
    void OnDisplayChanged();
    std::vector<MonitorInfo> CurrentMonitors() const;

    std::vector<std::wstring> ListShaders() const;
    std::wstring ShaderPathFor(int index) const;
    void AssignShaders();
    void SendShader(Slot& s);
    void RequestActiveShaders();
    void OnCompiled(std::unique_ptr<CompiledShader> r);

    void SetupWatchers();
    void OnWatcher(size_t i);
    void OnReloadTimer();
    bool ReloadConfig(bool force, bool quiet);
    void SaveConfigNow();
    void SyncAutostart();
    void Reload();

    void Evaluate(bool rescan, bool forceSend = false);
    void ScheduleScan(UINT delayMs);
    void OnPolicyTick();
    void OnGpuTick();
    void UpdateTimers();
    void SendGlobals();
    void SetEcoQos(bool on);

    void OnTray(WPARAM wp, LPARAM lp);
    void ShowMenu(POINT pt);
    void RunCommand(UINT cmd, const std::vector<std::wstring>& shaders);
    void UpdateStats();
    void UpdateTooltip(bool force);
    void LogBench();

    bool bench_ = false;
    bool shutDown_ = false;
    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;

    std::wstring exeDir_, configPath_, shadersDir_, cacheDir_;
    Config config_;
    uint64_t configHash_ = 0;
    std::string startupConfigError_;
    int configRetries_ = 0;
    bool pendingConfig_ = false, pendingShaders_ = false;

    Tray tray_;
    Renderer renderer_;
    bool rendererFailed_ = false;
    ShaderCompiler compiler_;
    DirWatcher watcher_;
    SessionWatcher session_;
    WindowEventHook winHook_;
    GpuMonitor gpu_;
    CpuLoad cpu_;
    ProcessUsage tipUsage_, benchUsage_;
    Policy policy_;

    DesktopHost host_;
    bool attached_ = false;
    int attachFailures_ = 0;
    std::vector<Slot> slots_;
    std::map<std::wstring, LoadedShader> shaders_;  // key: lowercase full path
    WindowScan scan_;

    bool userPaused_ = false;
    bool wasGloballyPaused_ = true;  // so the first evaluation always scans
    int forcedFps_ = 0;
    bool gameActive_ = false, busyActive_ = false;
    int gameHold_ = 0, busyHold_ = 0;
    GpuActivity lastGpu_;
    bool ecoQos_ = false;
    bool anyRunning_ = false;
    bool policyTimerOn_ = false, gpuTimerOn_ = false;
    bool scanScheduled_ = false;
    unsigned tick_ = 0;
    int64_t statsQpc_ = 0, benchQpc_ = 0;
    int64_t scanQpc_ = 0;  // time spent in ScanWindows, and scans, since the last bench report
    uint32_t scanCount_ = 0;
    ULONGLONG tipTime_ = 0;
};

// ---- lifecycle ----

int App::Run(bool bench) {
    bench_ = bench;
    if (!Init()) {
        Shutdown(false);
        return 1;
    }
    for (;;) {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS - 1];
        DWORD n = 0;
        for (size_t i = 0; i < watcher_.Count() && n < ARRAYSIZE(handles); ++i) handles[n++] = watcher_.Event(i);
        DWORD r = MsgWaitForMultipleObjectsEx(n, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r < WAIT_OBJECT_0 + n) {
            OnWatcher(r - WAIT_OBJECT_0);
            continue;
        }
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                Shutdown(true);
                return int(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

bool App::Init() {
    hinst_ = GetModuleHandleW(nullptr);
    exeDir_ = ExeDir();
    configPath_ = JoinPath(exeDir_, L"config.json");
    shadersDir_ = JoinPath(exeDir_, L"shaders");
    LogOpen(JoinPath(exeDir_, L"wallp.log"));
    Log("---- wallp starting%s ----", bench_ ? " (bench mode: auto-pause disabled)" : "");

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // for ShellExecute

    cacheDir_ = JoinPath(exeDir_, L"cache");
    if (!EnsureDir(cacheDir_)) {
        // Read-only install folder: keep the cache under %LOCALAPPDATA%.
        wchar_t buf[MAX_PATH];
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (len && len < MAX_PATH) {
            std::wstring base = JoinPath(buf, L"wallp");
            EnsureDir(base);
            cacheDir_ = JoinPath(base, L"cache");
            EnsureDir(cacheDir_);
        }
    }
    if (!DirExists(shadersDir_)) Log("warning: %s does not exist", Utf8(shadersDir_).c_str());

    if (!FileExists(configPath_) && WriteDefaultConfig(configPath_)) Log("created a default config.json");
    config_ = DefaultConfig();
    {
        std::string text, err;
        if (ReadFileBytes(configPath_, text)) {
            uint64_t hash = Fnv1a64(text.data(), text.size());
            if (ParseConfig(std::move(text), config_, err))
                configHash_ = hash;
            else
                startupConfigError_ = err;
        }
        if (!startupConfigError_.empty())
            Log("config.json: %s (using defaults)", startupConfigError_.c_str());
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst_;
    wc.lpszClassName = L"wallp.main";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so that broadcasts like TaskbarCreated
    // and WM_DISPLAYCHANGE arrive.
    CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"wallp", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hinst_,
                    this);
    if (!hwnd_) {
        Log("cannot create the main window (%lu)", GetLastError());
        return false;
    }
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);

    icon_ = static_cast<HICON>(LoadImageW(hinst_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!tray_.Create(hwnd_, WM_APP_TRAY, icon_)) Log("could not add the tray icon (waiting for the taskbar)");
    if (!startupConfigError_.empty())
        tray_.Balloon(L"config.json error", Wide(startupConfigError_) + L"\nUsing default settings.", true);

    if (!renderer_.Start(hwnd_, WM_APP_RENDER)) {
        MessageBoxW(nullptr, L"wallp could not create a Direct3D 11 device.", L"wallp", MB_ICONERROR | MB_OK);
        return false;
    }
    renderer_.SetReleaseAfterPause(config_.releaseAfterPauseSec);
    SendGlobals();
    compiler_.Start(hwnd_, WM_APP_COMPILED, cacheDir_, renderer_.ShaderProfile());

    session_.Register(hwnd_);
    winHook_.Install(&App::OnWindowEvent, this);
    SetupWatchers();
    SyncAutostart();
    Attach(true);
    if (bench_) {
        LogBench();  // baseline for the rates in the first report
        SetTimer(hwnd_, kTimerBench, 5000, nullptr);
    }
    return true;
}

void App::Shutdown(bool restoreWallpaper) {
    if (shutDown_) return;
    shutDown_ = true;
    Log("shutting down");
    if (hwnd_)
        for (UINT_PTR id = kTimerPolicy; id <= kTimerBench; ++id) KillTimer(hwnd_, id);
    winHook_.Uninstall();
    session_.Unregister();
    watcher_.Clear();
    compiler_.Stop();
    renderer_.Stop();  // destroys the wallpaper windows
    gpu_.Close();
    SetEcoQos(false);
    if (restoreWallpaper && attached_) RefreshDesktopWallpaper();
    tray_.Destroy();
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
    if (hwnd_) {
        HWND h = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(h);
    }
    CoUninitialize();
}

// ---- window messages ----

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::OnWindowEvent(void* p) {
    auto* self = static_cast<App*>(p);
    if (!self->scanScheduled_) self->ScheduleScan(100);
}

LRESULT App::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreated_ && msg == taskbarCreated_) {
        Log("taskbar created (Explorer started or restarted)");
        tray_.Recreate();
        SetTimer(hwnd_, kTimerReattach, 500, nullptr);
        return 0;
    }

    SessionState before = session_.State();
    if (session_.HandleMessage(msg, wp, lp)) {
        const SessionState& now = session_.State();
        if (now.locked != before.locked) Log("session %s", now.locked ? "locked" : "unlocked");
        if (now.disconnected != before.disconnected)
            Log("session %s", now.disconnected ? "disconnected" : "connected");
        if (now.displayOff != before.displayOff) Log("display %s", now.displayOff ? "off" : "on");
        if (now.suspended != before.suspended) Log("system %s", now.suspended ? "suspending" : "resumed");
        if (now.onBattery != before.onBattery) Log("power: %s", now.onBattery ? "battery" : "AC");
        Evaluate(true);
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        OnTray(wp, lp);
        return 0;
    case WM_APP_COMPILED:
        OnCompiled(std::unique_ptr<CompiledShader>(reinterpret_cast<CompiledShader*>(lp)));
        return 0;
    case WM_APP_RENDER:
        if (wp == Renderer::kTargetLost && attached_) {
            attached_ = false;
            SetTimer(hwnd_, kTimerReattach, 1000, nullptr);
        } else if (wp == Renderer::kDeviceFailed) {
            rendererFailed_ = true;
            tray_.Balloon(L"wallp", L"The GPU device was lost repeatedly, so rendering stopped. Choose Reload to try again.",
                          true);
        }
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        SetTimer(hwnd_, kTimerDisplay, 300, nullptr);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETWORKAREA) SetTimer(hwnd_, kTimerDisplay, 300, nullptr);
        return 0;
    case WM_ENDSESSION:
        if (wp) Shutdown(false);
        return 0;
    case WM_CLOSE:  // e.g. taskkill without /F: exit through the normal path
        PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        if (!shutDown_) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void App::OnTimer(UINT_PTR id) {
    switch (id) {
    case kTimerPolicy:
        OnPolicyTick();
        break;
    case kTimerScan:
        KillTimer(hwnd_, kTimerScan);
        scanScheduled_ = false;
        Evaluate(true);
        break;
    case kTimerGpu:
        OnGpuTick();
        break;
    case kTimerReload:
        KillTimer(hwnd_, kTimerReload);
        OnReloadTimer();
        break;
    case kTimerDisplay:
        KillTimer(hwnd_, kTimerDisplay);
        OnDisplayChanged();
        break;
    case kTimerReattach:
        KillTimer(hwnd_, kTimerReattach);
        Attach(false);
        break;
    case kTimerBench:
        LogBench();
        break;
    }
}

// ---- desktop / monitors ----

std::vector<MonitorInfo> App::CurrentMonitors() const {
    std::vector<MonitorInfo> mons;
    for (const Slot& s : slots_) mons.push_back(s.mon);
    return mons;
}

void App::Attach(bool force) {
    DesktopHost host;
    if (!FindDesktopHost(host)) {
        if (attachFailures_++ == 0) Log("desktop WorkerW not found; retrying every second");
        attached_ = false;
        SetTimer(hwnd_, kTimerReattach, 1000, nullptr);
        return;
    }
    attachFailures_ = 0;
    std::vector<MonitorInfo> monitors = EnumMonitors();
    if (!force && attached_ && host.parent == host_.parent && SameMonitorLayout(monitors, CurrentMonitors())) return;

    host_ = host;
    Log("desktop: %s (parent %p), %zu monitor(s)", host.layout, static_cast<void*>(host.parent), monitors.size());
    std::vector<RenderTargetDesc> descs;
    std::vector<Slot> slots;
    for (const MonitorInfo& m : monitors) {
        if (m.index >= kMaxMonitors) break;
        POINT pt{m.rect.left, m.rect.top};
        ScreenToClient(host.parent, &pt);
        descs.push_back({m.index, m.rect, pt, m.hz});
        Log("  monitor %d: %s %ldx%ld at (%ld,%ld), %d Hz%s", m.index, Utf8(m.device).c_str(),
            m.rect.right - m.rect.left, m.rect.bottom - m.rect.top, m.rect.left, m.rect.top, m.hz,
            m.primary ? ", primary" : "");
        Slot s;
        s.mon = m;
        if (size_t(m.index) < slots_.size()) {
            // The renderer keeps shader and run state per index across rebuilds.
            const Slot& old = slots_[size_t(m.index)];
            s.shaderPath = old.shaderPath;
            s.shaderKey = old.shaderKey;
            s.shaderSent = old.shaderSent;
            s.shaderMaxFps = old.shaderMaxFps;
        }
        slots.push_back(std::move(s));
    }
    slots_ = std::move(slots);
    renderer_.SetTargets(host.parent, host.insertAfter, std::move(descs));
    attached_ = true;
    AssignShaders();
    Evaluate(true, true);
}

void App::OnDisplayChanged() {
    std::vector<MonitorInfo> monitors = EnumMonitors();
    if (attached_ && SameMonitorLayout(monitors, CurrentMonitors())) {
        // Only work areas (taskbar) changed: no rebuild needed.
        for (size_t i = 0; i < slots_.size(); ++i) {
            slots_[i].mon.work = monitors[i].work;
            slots_[i].mon.handle = monitors[i].handle;
        }
        Evaluate(true);
        return;
    }
    Log("display configuration changed");
    Attach(true);
}

// ---- shaders ----

std::vector<std::wstring> App::ListShaders() const {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(shadersDir_, L"*.hlsl").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        std::wstring name = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.empty() || name[0] == L'_') continue;
        if (!EndsWith(Lower(name), L".hlsl")) continue;  // "_name.hlsl" and *.hlsli are include files
        names.push_back(std::move(name));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return names;
}

std::wstring App::ShaderPathFor(int index) const {
    // Exact entry, else the lowest configured monitor's shader, else the first shader found.
    const std::wstring* rel = nullptr;
    if (auto it = config_.monitors.find(index); it != config_.monitors.end())
        rel = &it->second;
    else if (!config_.monitors.empty())
        rel = &config_.monitors.begin()->second;
    if (rel && !rel->empty()) return ResolvePath(exeDir_, *rel);
    std::vector<std::wstring> list = ListShaders();
    return list.empty() ? std::wstring() : FullPath(JoinPath(shadersDir_, list.front()));
}

void App::AssignShaders() {
    for (Slot& s : slots_) {
        std::wstring path = ShaderPathFor(s.mon.index);
        std::wstring key = Lower(path);
        if (key != s.shaderKey) {
            s.shaderKey = key;
            s.shaderPath = path;
            s.shaderSent = false;
        }
        if (s.shaderSent) continue;
        if (key.empty()) {
            Log("monitor %d: no shader configured and none found in shaders/", s.mon.index);
            SendShader(s);
            continue;
        }
        auto it = shaders_.find(key);
        if (it != shaders_.end() && it->second.bytecode)
            SendShader(s);
        else
            compiler_.Request(path);  // OnCompiled sends it; the old shader stays until then
    }
    for (auto it = shaders_.begin(); it != shaders_.end();) {
        bool used = std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.shaderKey == it->first; });
        it = used ? std::next(it) : shaders_.erase(it);
    }
}

void App::SendShader(Slot& s) {
    RenderShader rs;
    s.shaderMaxFps = 0;
    auto it = shaders_.find(s.shaderKey);
    if (it != shaders_.end() && it->second.bytecode) {
        const LoadedShader& ls = it->second;
        rs.bytecode = ls.bytecode;
        rs.renderScale = config_.renderScale;
        if (ls.settings.renderScale > 0.0f) rs.renderScale = std::min(rs.renderScale, ls.settings.renderScale);
        rs.mouse = ls.settings.mouse;
        s.shaderMaxFps = ls.settings.maxFps;
    }
    renderer_.SetShader(s.mon.index, std::move(rs));
    s.shaderSent = true;
}

void App::RequestActiveShaders() {
    std::vector<std::wstring> seen;
    for (const Slot& s : slots_) {
        if (s.shaderKey.empty() || std::find(seen.begin(), seen.end(), s.shaderKey) != seen.end()) continue;
        seen.push_back(s.shaderKey);
        compiler_.Request(s.shaderPath);
    }
}

void App::OnCompiled(std::unique_ptr<CompiledShader> r) {
    if (shutDown_) return;
    std::wstring key = Lower(r->path);
    std::string name = Utf8(FileNameOf(r->path));
    bool used = std::any_of(slots_.begin(), slots_.end(), [&](const Slot& s) { return s.shaderKey == key; });
    if (!r->ok) {
        Log("shader %s failed:\n%s", name.c_str(), r->error.c_str());
        if (used) {
            auto it = shaders_.find(key);
            bool keeping = it != shaders_.end() && it->second.bytecode;
            tray_.Balloon(L"Shader error: " + Wide(name),
                          Wide(FirstLine(r->error)) + (keeping ? L"\n(keeping the previous version)" : L""), true);
        }
        return;
    }
    if (!used) return;  // the monitor switched to another shader meanwhile
    LoadedShader& ls = shaders_[key];
    bool reload = ls.bytecode != nullptr;
    if (reload && ls.hash == r->hash && ls.settings == r->settings) return;  // nothing effective changed
    Log("shader %s: %s in %.0f ms%s", name.c_str(), r->fromCache ? "loaded from cache" : "compiled", r->ms,
        reload ? " (reloaded)" : "");
    ls.hash = r->hash;
    ls.bytecode = std::move(r->bytecode);
    ls.settings = r->settings;
    for (Slot& s : slots_)
        if (s.shaderKey == key) SendShader(s);
    Evaluate(false);  // the sidecar can change the fps cap
}

// ---- config / files ----

void App::SetupWatchers() {
    watcher_.Clear();
    if (!watcher_.Watch(exeDir_, false, kWatchRoot)) Log("cannot watch %s", Utf8(exeDir_).c_str());
    if (DirExists(shadersDir_) && !watcher_.Watch(shadersDir_, true, kWatchShaders))
        Log("cannot watch %s", Utf8(shadersDir_).c_str());
}

void App::OnWatcher(size_t i) {
    std::vector<DirWatcher::Change> changes;
    watcher_.Collect(i, changes);
    bool relevant = false;
    for (const auto& c : changes) {
        std::wstring name = Lower(FileNameOf(c.path));
        if (c.tag == kWatchRoot) {
            if (c.path.empty() || name == L"config.json") relevant = pendingConfig_ = true;
        } else if (c.path.empty() || EndsWith(name, L".hlsl") || EndsWith(name, L".hlsli") ||
                   EndsWith(name, L".json") || EndsWith(name, L".fxh") || EndsWith(name, L".h")) {
            relevant = pendingShaders_ = true;
        }
    }
    if (relevant) {
        configRetries_ = 0;
        SetTimer(hwnd_, kTimerReload, 200, nullptr);  // restarts the 200 ms debounce
    }
}

void App::OnReloadTimer() {
    if (pendingConfig_) {
        pendingConfig_ = false;
        // An editor may still be writing: retry once quietly before reporting an error.
        bool quiet = configRetries_ == 0;
        if (!ReloadConfig(false, quiet) && quiet) {
            ++configRetries_;
            pendingConfig_ = true;
            SetTimer(hwnd_, kTimerReload, 500, nullptr);
        }
    }
    if (pendingShaders_) {
        pendingShaders_ = false;
        RequestActiveShaders();  // unchanged sources hash the same and are skipped
    }
}

bool App::ReloadConfig(bool force, bool quiet) {
    std::string text;
    if (!ReadFileBytes(configPath_, text)) {
        if (!quiet) Log("cannot read config.json");
        return false;
    }
    uint64_t hash = Fnv1a64(text.data(), text.size());
    if (!force && hash == configHash_) return true;  // e.g. our own write
    Config c;
    std::string err;
    if (!ParseConfig(std::move(text), c, err)) {
        if (!quiet) {
            Log("config.json: %s (keeping the previous settings)", err.c_str());
            tray_.Balloon(L"config.json error", Wide(err), true);
        }
        return false;
    }
    configHash_ = hash;
    bool scaleChanged = c.renderScale != config_.renderScale;
    config_ = std::move(c);
    Log("config reloaded");
    renderer_.SetReleaseAfterPause(config_.releaseAfterPauseSec);
    SyncAutostart();
    if (scaleChanged)
        for (Slot& s : slots_)
            if (s.shaderSent) SendShader(s);
    AssignShaders();
    Evaluate(false, true);
    return true;
}

void App::SaveConfigNow() {
    std::string err;
    if (!SaveConfig(configPath_, config_, err)) {
        Log("saving config.json failed: %s", err.c_str());
        tray_.Balloon(L"wallp", Wide(err), true);
        return;
    }
    ReloadConfig(true, false);  // keep memory identical to the file (and skip the watcher echo)
}

void App::SyncAutostart() {
    if (IsAutostartEnabled() == config_.autostart) return;
    if (SetAutostart(config_.autostart))
        Log("autostart %s", config_.autostart ? "enabled" : "disabled");
    else
        Log("cannot update the autostart Run key");
}

void App::Reload() {
    Log("reload requested");
    ReloadConfig(true, false);
    SetupWatchers();
    if (rendererFailed_) {
        renderer_.Stop();
        rendererFailed_ = !renderer_.Start(hwnd_, WM_APP_RENDER);
        if (!rendererFailed_) {
            renderer_.SetReleaseAfterPause(config_.releaseAfterPauseSec);
            SendGlobals();
        }
    }
    shaders_.clear();  // recompile (or reload from the cache) everything
    for (Slot& s : slots_) {
        s.shaderSent = false;
        s.decisionSent = false;
    }
    Attach(true);
}

// ---- policy ----

void App::Evaluate(bool rescan, bool forceSend) {
    if (shutDown_) return;
    const SessionState& ss = session_.State();
    PolicyInput in;
    in.userPaused = userPaused_;
    in.suspended = ss.suspended;
    in.locked = ss.locked || ss.disconnected;
    in.displayOff = ss.displayOff;
    in.onBattery = ss.onBattery;
    in.userIdle = config_.idleTimeoutSec > 0 && UserIdleMs() >= uint32_t(config_.idleTimeoutSec) * 1000u;
    in.game = gameActive_;
    in.busy = busyActive_;
    in.forcedFps = forcedFps_;
    in.noAutoPause = bench_;

    // Paused for every monitor anyway (user away included): a window scan could not change anything.
    bool globallyPaused =
        in.userPaused || in.suspended || in.locked || in.displayOff || (in.userIdle && !in.noAutoPause);
    if (wasGloballyPaused_ && !globallyPaused) rescan = true;  // the last scan predates the pause
    wasGloballyPaused_ = globallyPaused;
    if (rescan && !globallyPaused && !slots_.empty()) {
        int64_t t0 = QpcNow();
        scan_ = ScanWindows(CurrentMonitors());
        scanQpc_ += QpcNow() - t0;
        ++scanCount_;
    }
    for (size_t i = 0; i < slots_.size(); ++i) {
        PolicyInput::Monitor m;
        m.hz = slots_[i].mon.hz;
        m.shaderMaxFps = slots_[i].shaderMaxFps;
        m.fullscreen = i < scan_.fullscreen.size() && scan_.fullscreen[i];
        m.covered = i < scan_.covered.size() && scan_.covered[i];
        in.monitors.push_back(m);
    }

    int64_t recheck = -1;
    std::vector<MonitorDecision> decisions =
        policy_.Evaluate(in, config_, int64_t(GetTickCount64()), recheck);
    bool anyRunning = false, gameTier = false;
    for (size_t i = 0; i < slots_.size(); ++i) {
        Slot& s = slots_[i];
        const MonitorDecision& d = decisions[i];
        if (forceSend || !s.decisionSent || !(d == s.decision)) {
            const MonitorDecision& old = s.decision;
            bool changed = d.paused ? !s.decisionSent || !old.paused || d.reason != old.reason
                                    : !s.decisionSent || old.paused || d.fps != old.fps || d.tier != old.tier;
            if (changed && d.paused)
                Log("monitor %d: paused (%s)", s.mon.index, ReasonName(d.reason));
            else if (changed)
                Log("monitor %d: running at %d fps (%s tier%s)", s.mon.index, d.fps, TierName(d.tier),
                    d.batteryCapped ? ", battery cap" : "");
            // Only when the desktop cannot be seen may the renderer drop the frozen frame.
            bool hidden = d.paused && d.reason != PauseReason::User && d.reason != PauseReason::Inactive;
            s.decision = d;
            s.decisionSent = true;
            renderer_.SetRunState(s.mon.index, d.paused, d.fps, hidden);
        }
        anyRunning |= !d.paused;
        gameTier |= !d.paused && d.tier == Tier::Game;
    }
    if (recheck >= 0) ScheduleScan(UINT(std::max<int64_t>(recheck, 10)));

    bool wasRunning = anyRunning_;
    anyRunning_ = anyRunning;
    if (anyRunning && !wasRunning) SendGlobals();  // fresh date/battery after a pause
    SetEcoQos(gameTier || (anyRunning && in.onBattery));
    UpdateTimers();
}

void App::ScheduleScan(UINT delayMs) {
    SetTimer(hwnd_, kTimerScan, delayMs, nullptr);
    scanScheduled_ = true;
}

void App::OnPolicyTick() {
    if (attached_ && !IsWindow(host_.parent)) {
        Log("desktop window disappeared; reattaching");
        attached_ = false;
        Attach(true);
        return;
    }
    Evaluate(true);
    if ((++tick_ & 1) == 0) {  // about once a second
        if (anyRunning_) SendGlobals();
        UpdateStats();
    }
}

void App::OnGpuTick() {
    GpuActivity a = gpu_.Sample(config_.games, config_.ignore, config_.video, config_.gpuGameThreshold);
    if (!a.valid) return;
    lastGpu_ = a;
    // Enter a tier on the first sample, leave it after two quiet ones.
    gameHold_ = a.game ? 2 : std::max(0, gameHold_ - 1);
    busyHold_ = a.busy ? 2 : std::max(0, busyHold_ - 1);
    bool game = gameHold_ > 0, busy = busyHold_ > 0;
    if (game == gameActive_ && busy == busyActive_) return;
    gameActive_ = game;
    busyActive_ = busy;
    Log("GPU activity: %s (busiest: %s at %.0f%%)", game ? "game" : busy ? "busy" : "normal",
        a.top.empty() ? "-" : Utf8(a.top).c_str(), a.topLoad * 100.0);
    Evaluate(false);
}

void App::UpdateTimers() {
    if (!hwnd_ || shutDown_) return;
    const SessionState& ss = session_.State();
    // While paused by the user, lock, display off or sleep only an event can resume us.
    bool policy = !(userPaused_ || ss.locked || ss.disconnected || ss.displayOff || ss.suspended);
    if (policy != policyTimerOn_) {
        policyTimerOn_ = policy;
        if (policy)
            SetTimer(hwnd_, kTimerPolicy, 500, nullptr);
        else
            KillTimer(hwnd_, kTimerPolicy);
    }
    // PDH sampling has a cost of its own: only while something renders.
    if (anyRunning_ != gpuTimerOn_) {
        gpuTimerOn_ = anyRunning_;
        if (gpuTimerOn_) {
            SetTimer(hwnd_, kTimerGpu, 2000, nullptr);
            gpu_.Sample(config_.games, config_.ignore, config_.video, config_.gpuGameThreshold);  // prime
        } else {
            KillTimer(hwnd_, kTimerGpu);
            gpu_.Unprime();
        }
    }
}

void App::SendGlobals() {
    RenderGlobals g;
    SYSTEMTIME st;
    GetLocalTime(&st);
    g.year = float(st.wYear);
    g.month = float(st.wMonth - 1);  // Shadertoy: 0-11
    g.day = float(st.wDay);
    g.secondsOfDay = st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond + st.wMilliseconds / 1000.0;
    g.refQpc = QpcNow();
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        bool noBattery = (ps.BatteryFlag & 128) || ps.BatteryFlag == 255 || ps.BatteryLifePercent > 100;
        g.battery = noBattery ? -1.0f : ps.BatteryLifePercent / 100.0f;
        g.onBattery = ps.ACLineStatus == 0 ? 1.0f : 0.0f;
    }
    g.cpuLoad = cpu_.Sample();
    renderer_.SetGlobals(g);
}

void App::SetEcoQos(bool on) {
    if (on == ecoQos_) return;
    ecoQos_ = on;
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;  // 0: let Windows decide again
    s.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof(s));
    Log("EcoQoS %s", on ? "on" : "off");
}

// ---- tray / stats ----

void App::OnTray(WPARAM wp, LPARAM lp) {
    switch (LOWORD(lp)) {
    case WM_MOUSEMOVE:
        UpdateTooltip(false);  // stats are only computed while someone looks at them
        break;
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case WM_CONTEXTMENU:
        ShowMenu(POINT{GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
        break;
    }
}

void App::ShowMenu(POINT pt) {
    std::vector<std::wstring> shaders = ListShaders();
    TrayMenuModel m;
    for (const Slot& s : slots_) {
        TrayMenuModel::Monitor mm;
        wchar_t label[128];
        swprintf_s(label, L"Monitor %d  (%ldx%ld%s)", s.mon.index, s.mon.rect.right - s.mon.rect.left,
                   s.mon.rect.bottom - s.mon.rect.top, s.mon.primary ? L", primary" : L"");
        mm.label = label;
        mm.shaders = shaders;
        for (size_t i = 0; i < shaders.size(); ++i)
            if (Lower(FullPath(JoinPath(shadersDir_, shaders[i]))) == s.shaderKey) mm.selected = int(i);
        m.monitors.push_back(std::move(mm));
    }
    m.paused = userPaused_;
    m.forcedFps = forcedFps_;
    m.showStats = config_.showStats;
    m.autostart = config_.autostart;
    m.theme = config_.theme;
    UINT cmd = ShowTrayMenu(hwnd_, m, pt);
    if (cmd != kCmdNone) RunCommand(cmd, shaders);
}

void App::RunCommand(UINT cmd, const std::vector<std::wstring>& shaders) {
    switch (cmd) {
    case kCmdPauseToggle:
        userPaused_ = !userPaused_;
        Log(userPaused_ ? "paused by user" : "resumed by user");
        Evaluate(true);
        return;
    case kCmdFpsAuto:
    case kCmdFps120:
    case kCmdFps60:
    case kCmdFps30:
    case kCmdFps15:
        forcedFps_ = cmd == kCmdFps120 ? 120 : cmd == kCmdFps60 ? 60 : cmd == kCmdFps30 ? 30 : cmd == kCmdFps15 ? 15 : 0;
        Log("FPS tier: %s", forcedFps_ ? std::to_string(forcedFps_).c_str() : "auto");
        Evaluate(false);
        return;
    case kCmdOpenConfig:
        OpenInShell(configPath_, true);
        return;
    case kCmdOpenShaders:
        OpenInShell(shadersDir_, false);
        return;
    case kCmdReload:
        Reload();
        return;
    case kCmdShowStats:
        config_.showStats = !config_.showStats;
        SaveConfigNow();
        UpdateTooltip(true);
        return;
    case kCmdAutostart:
        config_.autostart = !config_.autostart;
        SyncAutostart();
        SaveConfigNow();
        return;
    case kCmdThemeSystem:
    case kCmdThemeLight:
    case kCmdThemeDark:
        config_.theme = int(cmd - kCmdThemeSystem);
        SaveConfigNow();
        return;
    case kCmdExit:
        PostQuitMessage(0);
        return;
    }
    if (cmd >= kCmdShaderBase) {
        size_t mon = (cmd - kCmdShaderBase) / 256, idx = (cmd - kCmdShaderBase) % 256;
        if (mon >= slots_.size() || idx >= shaders.size()) return;
        int index = slots_[mon].mon.index;
        config_.monitors[index] = L"shaders/" + shaders[idx];
        Log("monitor %d: shader set to %s", index, Utf8(shaders[idx]).c_str());
        SaveConfigNow();
        AssignShaders();
    }
}

void App::UpdateStats() {
    int64_t now = QpcNow();
    double dt = statsQpc_ ? QpcToSec(now - statsQpc_) : 0.0;
    statsQpc_ = now;
    for (Slot& s : slots_) {
        uint64_t frames = renderer_.Stats(s.mon.index).frames.load();
        s.fps = dt > 0 ? double(frames - s.lastFrames) / dt : 0.0;
        s.lastFrames = frames;
    }
}

void App::UpdateTooltip(bool force) {
    ULONGLONG now = GetTickCount64();
    if (!force && now - tipTime_ < 1000) return;
    tipTime_ = now;
    if (!config_.showStats) {
        tray_.SetTip(L"wallp");
        return;
    }
    double cpu = 0, mem = 0;
    tipUsage_.Sample(cpu, mem);
    wchar_t line[128];
    swprintf_s(line, L"wallp  CPU %.1f%%  RAM %.0f MB", cpu, mem);
    std::wstring tip = line;
    for (const Slot& s : slots_) {
        const MonitorDecision& d = s.decision;
        if (d.paused)
            swprintf_s(line, L"\n%d: paused (%hs)", s.mon.index, ReasonName(d.reason));
        else
            swprintf_s(line, L"\n%d: %.0f fps, cap %d (%hs%hs)", s.mon.index, s.fps, d.fps, TierName(d.tier),
                       d.batteryCapped ? ", battery" : "");
        tip += line;
    }
    if (tip.size() > 127) tip.resize(127);
    tray_.SetTip(tip);
}

void App::LogBench() {
    double cpu = 0, mem = 0;
    benchUsage_.Sample(cpu, mem);
    int64_t now = QpcNow();
    double dt = benchQpc_ ? QpcToSec(now - benchQpc_) : 0.0;
    benchQpc_ = now;
    double events = WindowEventHook::TakeEventCount();
    char buf[192];
    snprintf(buf, sizeof(buf), "bench: cpu %.2f%%  ram %.1f MB  winevents %.0f/s  scans %.1f/s at %.0f us", cpu,
             mem, dt > 0 ? events / dt : 0.0, dt > 0 ? scanCount_ / dt : 0.0,
             scanCount_ ? QpcToSec(scanQpc_) * 1e6 / scanCount_ : 0.0);
    scanQpc_ = 0;
    scanCount_ = 0;
    std::string line = buf;
    for (Slot& s : slots_) {
        uint64_t frames = renderer_.Stats(s.mon.index).frames.load();
        double fps = dt > 0 ? double(frames - s.benchFrames) / dt : 0.0;
        s.benchFrames = frames;
        snprintf(buf, sizeof(buf), " | m%d %.1f fps (%s, cap %d)", s.mon.index, fps,
                 s.decision.paused ? ReasonName(s.decision.reason) : TierName(s.decision.tier), s.decision.fps);
        line += buf;
    }
    if (lastGpu_.valid) {
        snprintf(buf, sizeof(buf), " | gpu: wallp %.1f%%, busiest other %s %.0f%%", lastGpu_.selfLoad * 100.0,
                 lastGpu_.top.empty() ? "-" : Utf8(lastGpu_.top).c_str(), lastGpu_.topLoad * 100.0);
        line += buf;
    }
    if (dt > 0) Log("%s", line.c_str());  // the first call only sets the baseline
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    bool bench = cmdLine && wcsstr(cmdLine, L"--bench") != nullptr;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\wallp.single-instance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"wallp is already running. Use its tray icon to change settings or exit.", L"wallp",
                    MB_ICONINFORMATION | MB_OK);
        CloseHandle(mutex);
        return 0;
    }
    int rc = 0;
    {
        App app;
        rc = app.Run(bench);
    }
    if (mutex) CloseHandle(mutex);
    return rc;
}
