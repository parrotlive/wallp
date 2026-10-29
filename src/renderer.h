#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

constexpr int kMaxMonitors = 16;

struct RenderTargetDesc {
    int index = 0;
    RECT screen{};      // monitor rect, virtual-desktop coordinates
    POINT clientPos{};  // the same rect's top-left in the parent's client coordinates
    int hz = 60;
};

struct RenderShader {
    std::shared_ptr<const std::vector<uint8_t>> bytecode;  // null = nothing to draw
    float renderScale = 1.0f;
    bool mouse = false;
};

// Slow-changing uniform values, refreshed by the main thread about once a second.
struct RenderGlobals {
    float year = 0, month = 0, day = 0;
    double secondsOfDay = 0;  // at refQpc; the render thread extrapolates
    int64_t refQpc = 0;
    float battery = -1, onBattery = 0, cpuLoad = 0;
};

struct MonitorStats {
    std::atomic<uint64_t> frames{0};
    std::atomic<int> width{0}, height{0}, renderWidth{0}, renderHeight{0};
    std::atomic<bool> hasSwapchain{false};
};

// Owns the render thread: one D3D11 device, and per monitor a child window of the desktop
// WorkerW with a flip-model swapchain. The thread blocks in MsgWaitForMultipleObjectsEx on
// a command event, one high-resolution waitable timer and the swapchain waitable objects;
// a paused monitor contributes nothing to that wait.
class Renderer {
public:
    enum Notify : WPARAM { kTargetLost = 1, kDeviceFailed = 2 };

    Renderer();
    ~Renderer();  // out of line: Gpu and Target are only complete in renderer.cpp
    // Creates the thread and the device. Events are posted to `notify` as `msg`
    // (wParam = Notify, lParam = monitor index).
    bool Start(HWND notify, UINT msg);
    void Stop();
    const char* ShaderProfile() const { return profile_.load(); }

    void SetTargets(HWND parent, HWND insertAfter, std::vector<RenderTargetDesc> targets);
    void SetShader(int index, RenderShader shader);
    // `hidden`: the desktop on this monitor cannot be seen (covered, locked, ...), so after a
    // long pause the swapchain may be released; otherwise the last frame stays on screen.
    void SetRunState(int index, bool paused, int fps, bool hidden);
    void SetGlobals(const RenderGlobals& globals);
    void SetReleaseAfterPause(int seconds);
    MonitorStats& Stats(int index) { return stats_[index]; }

private:
    struct CmdTargets {
        HWND parent;
        HWND insertAfter;
        std::vector<RenderTargetDesc> targets;
    };
    struct CmdShader {
        int index;
        RenderShader shader;
    };
    struct CmdRun {
        int index;
        bool paused;
        int fps;
        bool hidden;
    };
    struct CmdGlobals {
        RenderGlobals globals;
    };
    struct CmdOptions {
        int releaseAfterPauseSec;
    };
    struct CmdQuit {};
    using Command = std::variant<CmdTargets, CmdShader, CmdRun, CmdGlobals, CmdOptions, CmdQuit>;

    struct Target;
    struct Gpu;

    void Post(Command cmd);
    void ThreadMain();
    void DrainCommands();
    void Apply(CmdTargets& c);
    void Apply(CmdShader& c);
    void Apply(CmdRun& c);
    void Apply(CmdGlobals& c);
    void Apply(CmdOptions& c);
    void Apply(CmdQuit&);

    bool CreateDevice();
    void DestroyDevice();
    void RecoverDevice();
    bool CreateSwapchain(Target& t);
    void ReleaseSwapchain(Target& t);
    void UpdateLowRes(Target& t);
    void CreatePixelShader(Target& t);
    void ApplyRunState(Target& t);
    void ConfigurePacing(Target& t);
    bool VsyncPacingHolds(Target& t, int64_t now);
    void RequestFrame(Target& t);
    void RenderFrame(Target& t);
    void DestroyTargets();
    void LoseTarget(Target& t);
    Target* FindTarget(int index);

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND notify_ = nullptr;
    UINT notifyMsg_ = 0;
    std::thread thread_;
    HANDLE cmdEvent_ = nullptr;
    HANDLE initEvent_ = nullptr;
    bool initOk_ = false;
    std::mutex mutex_;
    std::vector<Command> queue_;
    MonitorStats stats_[kMaxMonitors];
    std::atomic<const char*> profile_{"ps_5_0"};

    // ---- render-thread state ----
    std::unique_ptr<Gpu> gpu_;
    std::vector<std::unique_ptr<Target>> targets_;
    RenderShader shaders_[kMaxMonitors];
    struct RunState {
        bool paused = true;
        int fps = 30;
        bool hidden = false;
    } run_[kMaxMonitors];
    RenderGlobals globals_;
    int64_t releaseAfter_ = 0;  // QPC ticks, 0 = never
    HANDLE timer_ = nullptr;
    bool timerArmed_ = false;
    int64_t timerDeadline_ = 0;
    int64_t epoch_ = 0;  // timer-paced frames fall on multiples of their period after this
    bool quit_ = false;
    bool deviceLost_ = false;
    bool destroying_ = false;
    int64_t lossTimes_[3] = {};
};
