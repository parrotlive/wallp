#include "renderer.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

#include "builtin_blit.h"
#include "builtin_vs.h"
#include "util.h"

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t kWindowClass[] = L"wallp.wallpaper";

// Must match the cbuffer in the shader preamble (shader.cpp).
struct alignas(16) GpuGlobals {
    float iResolution[3];
    float iTime;
    float iTimeDelta;
    int32_t iFrame;
    float pad0[2];
    float iMouse[4];
    float iDate[4];
    float iBattery;
    float iOnBattery;
    float iCpuLoad;
    float iMonitor;
    float iMouseVel[2];
    float pad1[2];
    float iClick[4][4];
    float iTrail[16][4];
};
static_assert(sizeof(GpuGlobals) == 416, "GpuGlobals must match the HLSL cbuffer");

constexpr float kNoClickAge = 1e4f;  // iClick age (seconds) of a slot with no click yet

// Cursor history for shaders with "mouse": true, in screen pixels, newest first. Updated
// only on rendered frames, so a paused monitor costs nothing here.
struct MouseInput {
    static constexpr int kTrail = 16, kClicks = 4;
    static constexpr double kTrailStep = 1.0 / 30.0;  // seconds between trail samples

    struct Sample {
        POINT p{};
        int64_t qpc = 0;  // 0 = empty
    };
    Sample trail[kTrail];
    Sample clicks[kClicks];
    POINT last{};
    int64_t lastQpc = 0;
    float vx = 0, vy = 0;  // smoothed velocity, screen pixels per second
    bool prevDown = false, held = false, clickFrame = false;

    void Update(POINT p, bool down, const RECT& screen, int64_t now) {
        double dt = lastQpc ? QpcToSec(now - lastQpc) : 1e9;
        if (dt > 0.25) {
            // First frame, or back from a pause: no velocity, and the trail starts here.
            vx = vy = 0;
            for (int i = 0; i < kTrail; ++i) trail[i] = {p, now - SecToQpc((i + 1) * kTrailStep)};
        } else if (dt > 0) {
            float k = float(1.0 - std::exp(-dt / 0.06));
            vx += (float((p.x - last.x) / dt) - vx) * k;
            vy += (float((p.y - last.y) / dt) - vy) * k;
        }
        last = p;
        lastQpc = now;
        // 3/4 of a step: at 30 fps every frame samples, despite frame-time jitter.
        if (QpcToSec(now - trail[0].qpc) >= 0.75 * kTrailStep) {
            std::copy_backward(trail, trail + kTrail - 1, trail + kTrail);
            trail[0] = {p, now};
        }
        if (down && !prevDown && PtInRect(&screen, p)) {
            std::copy_backward(clicks, clicks + kClicks - 1, clicks + kClicks);
            clicks[0] = {p, now};
            held = clickFrame = true;
        }
        if (!down) held = false;
        prevDown = down;
    }

    // Render-target pixels (origin bottom-left) for a target of rw x rh showing `screen`.
    void Fill(GpuGlobals& g, const RECT& screen, int rw, int rh, int64_t now) {
        float sx = float(rw) / float(std::max<LONG>(1, screen.right - screen.left));
        float sy = float(rh) / float(std::max<LONG>(1, screen.bottom - screen.top));
        auto x = [&](POINT p) { return float(p.x - screen.left) * sx; };
        auto y = [&](POINT p) { return float(screen.bottom - p.y) * sy; };
        g.iMouse[0] = x(last);
        g.iMouse[1] = y(last);
        if (clicks[0].qpc) {  // Shadertoy: z > 0 while held, w > 0 on the click's first frame
            g.iMouse[2] = held ? x(clicks[0].p) : -x(clicks[0].p);
            g.iMouse[3] = clickFrame ? y(clicks[0].p) : -y(clicks[0].p);
        }
        clickFrame = false;
        g.iMouseVel[0] = vx * sx;
        g.iMouseVel[1] = -vy * sy;
        for (int i = 0; i < kClicks; ++i) {
            const Sample& c = clicks[i];
            if (!c.qpc) continue;
            g.iClick[i][0] = x(c.p);
            g.iClick[i][1] = y(c.p);
            g.iClick[i][2] = float(QpcToSec(now - c.qpc));
            g.iClick[i][3] = (i == 0 && held) ? 1.0f : 0.0f;
        }
        for (int i = 0; i < kTrail; ++i) {
            g.iTrail[i][0] = x(trail[i].p);
            g.iTrail[i][1] = y(trail[i].p);
            g.iTrail[i][2] = float(QpcToSec(now - trail[i].qpc));
        }
    }
};

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

}  // namespace

struct Renderer::Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> blit;
    ComPtr<ID3D11Buffer> cbuffer;
    ComPtr<ID3D11SamplerState> sampler;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
};

struct Renderer::Target {
    int index = 0;
    HWND hwnd = nullptr;
    RECT screen{};
    int width = 1, height = 1, hz = 60;

    ComPtr<IDXGISwapChain2> swapchain;
    HANDLE waitable = nullptr;  // frame-latency waitable object
    ComPtr<ID3D11RenderTargetView> rtv;

    // Reduced-resolution render target, only when renderScale < 1.
    float scale = 1.0f;
    int rw = 1, rh = 1;
    ComPtr<ID3D11Texture2D> lowTex;
    ComPtr<ID3D11RenderTargetView> lowRtv;
    ComPtr<ID3D11ShaderResourceView> lowSrv;

    ComPtr<ID3D11PixelShader> ps;
    bool mouse = false;

    bool paused = true;
    int fps = 30;
    bool hidden = false;      // the desktop here cannot be seen: the swapchain may be released
    int64_t hiddenSince = 0;  // start of the release countdown (restarted by any frame)
    bool oneShot = false;     // render one frame even though paused (new shader / new swapchain)

    // None: idle. Timer: waiting for nextDue. Swap: due, waiting for the latency waitable.
    enum class Wait { None, Timer, Swap } wait = Wait::None;
    bool vsyncPaced = false;  // fps divides the refresh rate: pace with SyncInterval, no timer
    bool vsyncUnreliable = false;  // vsync pacing overshot the cap once: timer only from now on
    UINT syncInterval = 1;
    int64_t period = 0;
    int64_t nextDue = 0;
    int64_t paceStart = 0;  // vsync pacing check: window start and frames in it
    int paceFrames = 0;

    double time = 0.0;
    int64_t lastFrame = 0;
    int frame = 0;

    MouseInput input;
};

// ---- public API (main thread) ----

Renderer::Renderer() = default;
Renderer::~Renderer() { Stop(); }

bool Renderer::Start(HWND notify, UINT msg) {
    notify_ = notify;
    notifyMsg_ = msg;
    quit_ = false;
    cmdEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    initEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this] { ThreadMain(); });
    WaitForSingleObject(initEvent_, INFINITE);
    CloseHandle(initEvent_);
    initEvent_ = nullptr;
    if (!initOk_) {
        thread_.join();
        CloseHandle(cmdEvent_);
        cmdEvent_ = nullptr;
        return false;
    }
    return true;
}

void Renderer::Stop() {
    if (!thread_.joinable()) return;
    Post(CmdQuit{});
    thread_.join();
    CloseHandle(cmdEvent_);
    cmdEvent_ = nullptr;
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
    }
    for (auto& s : stats_) {
        s.hasSwapchain = false;
        s.width = s.height = s.renderWidth = s.renderHeight = 0;
    }
}

void Renderer::Post(Command cmd) {
    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(cmd));
    }
    SetEvent(cmdEvent_);
}

void Renderer::SetTargets(HWND parent, HWND insertAfter, std::vector<RenderTargetDesc> targets) {
    Post(CmdTargets{parent, insertAfter, std::move(targets)});
}

void Renderer::SetShader(int index, RenderShader shader) {
    if (index >= 0 && index < kMaxMonitors) Post(CmdShader{index, std::move(shader)});
}

void Renderer::SetRunState(int index, bool paused, int fps, bool hidden) {
    if (index >= 0 && index < kMaxMonitors) Post(CmdRun{index, paused, fps, hidden});
}

void Renderer::SetGlobals(const RenderGlobals& globals) { Post(CmdGlobals{globals}); }

void Renderer::SetReleaseAfterPause(int seconds) { Post(CmdOptions{seconds}); }

// ---- render thread ----

void Renderer::ThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    // Our windows are children of Explorer's; allow a DPI awareness different from theirs.
    using SetHostingFn = DPI_HOSTING_BEHAVIOR(WINAPI*)(DPI_HOSTING_BEHAVIOR);
    if (auto fn = reinterpret_cast<SetHostingFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiHostingBehavior")))
        fn(DPI_HOSTING_BEHAVIOR_MIXED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    epoch_ = QpcNow();
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // before 1803
    initOk_ = timer_ && CreateDevice();
    SetEvent(initEvent_);
    if (!initOk_) {
        if (timer_) CloseHandle(timer_);
        timer_ = nullptr;
        return;
    }

    const int64_t tolerance = QpcFreq() / 1000;  // treat anything due within 1 ms as due now
    while (!quit_) {
        DrainCommands();
        if (quit_) break;
        if (deviceLost_) RecoverDevice();

        // Render whatever is due and ready, and find the next deadline.
        int64_t now = QpcNow();
        int64_t deadline = INT64_MAX;
        for (auto& tp : targets_) {
            Target& t = *tp;
            if (t.wait == Target::Wait::Timer && t.nextDue - now <= tolerance) t.wait = Target::Wait::Swap;
            if (t.wait == Target::Wait::Swap && WaitForSingleObject(t.waitable, 0) == WAIT_OBJECT_0)
                RenderFrame(t);
            if (deviceLost_) break;
            if (t.wait == Target::Wait::Timer) deadline = std::min(deadline, t.nextDue);
            if (t.paused && t.hidden && t.wait == Target::Wait::None && t.swapchain && releaseAfter_ > 0) {
                int64_t at = t.hiddenSince + releaseAfter_;
                if (at - now <= tolerance) {
                    ReleaseSwapchain(t);
                    Log("monitor %d: swapchain released after a long pause", t.index);
                } else
                    deadline = std::min(deadline, at);
            }
        }
        if (deviceLost_) continue;

        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        Target* owners[MAXIMUM_WAIT_OBJECTS] = {};
        DWORD n = 0;
        handles[n++] = cmdEvent_;
        for (auto& tp : targets_) {
            if (tp->wait == Target::Wait::Swap && n < MAXIMUM_WAIT_OBJECTS - 2) {
                owners[n] = tp.get();
                handles[n++] = tp->waitable;
            }
        }
        DWORD timerSlot = MAXDWORD;
        if (deadline != INT64_MAX) {
            now = QpcNow();
            if (deadline - now <= tolerance) continue;
            if (!timerArmed_ || timerDeadline_ != deadline) {
                LARGE_INTEGER due;
                due.QuadPart = -std::max<int64_t>(1, (deadline - now) * 10'000'000 / QpcFreq());
                timerArmed_ = SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0) != 0;
                timerDeadline_ = deadline;
            }
            timerSlot = n;
            handles[n++] = timer_;
        }

        DWORD r = MsgWaitForMultipleObjectsEx(n, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_OBJECT_0 + n) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        } else if (r < WAIT_OBJECT_0 + n) {
            DWORD i = r - WAIT_OBJECT_0;
            if (i == timerSlot)
                timerArmed_ = false;
            else if (owners[i])
                RenderFrame(*owners[i]);  // the wait consumed its latency waitable
        }
    }

    DestroyTargets();
    DestroyDevice();
    CloseHandle(timer_);
    timer_ = nullptr;
}

void Renderer::DrainCommands() {
    std::vector<Command> cmds;
    {
        std::lock_guard lock(mutex_);
        cmds.swap(queue_);
    }
    for (auto& c : cmds) std::visit([this](auto& cmd) { Apply(cmd); }, c);
}

void Renderer::Apply(CmdTargets& c) {
    DestroyTargets();
    for (const RenderTargetDesc& d : c.targets) {
        if (d.index < 0 || d.index >= kMaxMonitors) continue;
        auto t = std::make_unique<Target>();
        t->index = d.index;
        t->screen = d.screen;
        t->width = std::max<int>(1, d.screen.right - d.screen.left);
        t->height = std::max<int>(1, d.screen.bottom - d.screen.top);
        t->hz = std::max(1, d.hz);
        t->hwnd = CreateWindowExW(WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE, kWindowClass, L"wallp",
                                  WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, d.clientPos.x, d.clientPos.y, t->width,
                                  t->height, c.parent, nullptr, GetModuleHandleW(nullptr), this);
        if (!t->hwnd) {
            Log("monitor %d: CreateWindowEx failed (error %lu)", d.index, GetLastError());
            PostMessageW(notify_, notifyMsg_, kTargetLost, d.index);
            continue;
        }
        if (c.insertAfter)
            SetWindowPos(t->hwnd, c.insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(t->hwnd, SW_SHOWNA);
        stats_[d.index].width = t->width;
        stats_[d.index].height = t->height;

        Target& ref = *t;
        targets_.push_back(std::move(t));
        CreateSwapchain(ref);
        CreatePixelShader(ref);
        ApplyRunState(ref);
        RequestFrame(ref);  // show the shader right away, even if the monitor starts paused
    }
}

void Renderer::Apply(CmdShader& c) {
    shaders_[c.index] = std::move(c.shader);
    if (Target* t = FindTarget(c.index)) {
        CreatePixelShader(*t);
        ApplyRunState(*t);
        RequestFrame(*t);
    }
}

void Renderer::Apply(CmdRun& c) {
    run_[c.index] = {c.paused, std::max(1, c.fps), c.hidden};
    if (Target* t = FindTarget(c.index)) ApplyRunState(*t);
}

void Renderer::Apply(CmdGlobals& c) { globals_ = c.globals; }

void Renderer::Apply(CmdOptions& c) {
    releaseAfter_ = c.releaseAfterPauseSec > 0 ? SecToQpc(c.releaseAfterPauseSec) : 0;
}

void Renderer::Apply(CmdQuit&) { quit_ = true; }

Renderer::Target* Renderer::FindTarget(int index) {
    for (auto& t : targets_)
        if (t->index == index) return t.get();
    return nullptr;
}

bool Renderer::CreateDevice() {
    auto gpu = std::make_unique<Gpu>();
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                               D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    // Only this thread touches the device; also ask the driver not to spin up worker threads.
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_DRIVER_TYPE type = D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = E_FAIL;
    for (;;) {
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &gpu->device, &gpu->level, &gpu->ctx);
        if (SUCCEEDED(hr)) break;
        if (flags & D3D11_CREATE_DEVICE_DEBUG) {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;  // SDK layers not installed
        } else if (flags & D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS) {
            flags &= ~D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
        } else if (type == D3D_DRIVER_TYPE_HARDWARE) {
            Log("no hardware D3D11 device (0x%08lX), falling back to WARP", static_cast<unsigned long>(hr));
            type = D3D_DRIVER_TYPE_WARP;
        } else {
            Log("D3D11CreateDevice failed (0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
    }

    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(gpu->device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&gpu->factory)))) {
        Log("cannot get the DXGI factory");
        return false;
    }
    dxgiDevice->SetMaximumFrameLatency(1);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    Log("D3D11 device: %s, feature level %d_%d", Utf8(ad.Description).c_str(), (gpu->level >> 12) & 0xF,
        (gpu->level >> 8) & 0xF);

    gpu->device->CreateVertexShader(g_builtinVS, sizeof(g_builtinVS), nullptr, &gpu->vs);
    gpu->device->CreatePixelShader(g_builtinBlitPS, sizeof(g_builtinBlitPS), nullptr, &gpu->blit);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(GpuGlobals);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    gpu->device->CreateBuffer(&bd, nullptr, &gpu->cbuffer);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    gpu->device->CreateSamplerState(&sd, &gpu->sampler);
    if (!gpu->vs || !gpu->blit || !gpu->cbuffer || !gpu->sampler) {
        Log("pipeline setup failed");
        return false;
    }

    // State that never changes is bound once; Present only unbinds the back buffer.
    ID3D11DeviceContext* ctx = gpu->ctx.Get();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(gpu->vs.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, gpu->cbuffer.GetAddressOf());
    ctx->PSSetSamplers(0, 1, gpu->sampler.GetAddressOf());

    profile_ = gpu->level >= D3D_FEATURE_LEVEL_11_0 ? "ps_5_0" : "ps_4_0";
    gpu_ = std::move(gpu);
    return true;
}

void Renderer::DestroyDevice() {
    if (!gpu_) return;
    if (gpu_->ctx) {
        gpu_->ctx->ClearState();
        gpu_->ctx->Flush();
    }
    gpu_.reset();
}

void Renderer::RecoverDevice() {
    deviceLost_ = false;
    HRESULT reason = gpu_ ? gpu_->device->GetDeviceRemovedReason() : E_FAIL;
    Log("GPU device lost (reason 0x%08lX)", static_cast<unsigned long>(reason));

    // Three losses within a minute (say, a shader that trips the GPU watchdog): stop.
    int64_t now = QpcNow();
    lossTimes_[0] = lossTimes_[1];
    lossTimes_[1] = lossTimes_[2];
    lossTimes_[2] = now;
    bool storm = lossTimes_[0] != 0 && now - lossTimes_[0] < SecToQpc(60);

    for (auto& t : targets_) {
        ReleaseSwapchain(*t);
        t->ps.Reset();
    }
    DestroyDevice();
    if (storm || !CreateDevice()) {
        Log("giving up on rendering until reload");
        PostMessageW(notify_, notifyMsg_, kDeviceFailed, 0);
        return;
    }
    for (auto& t : targets_) {
        CreateSwapchain(*t);
        CreatePixelShader(*t);
        t->paused = true;  // forces ApplyRunState to restart pacing
        ApplyRunState(*t);
        RequestFrame(*t);
    }
}

bool Renderer::CreateSwapchain(Target& t) {
    if (t.swapchain) return true;
    if (!gpu_ || !t.hwnd) return false;
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = UINT(t.width);
    d.Height = UINT(t.height);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = gpu_->factory->CreateSwapChainForHwnd(gpu_->device.Get(), t.hwnd, &d, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&t.swapchain))) {
        Log("monitor %d: CreateSwapChainForHwnd failed (0x%08lX)", t.index, static_cast<unsigned long>(hr));
        if (IsDeviceLost(hr)) deviceLost_ = true;
        t.swapchain.Reset();
        return false;
    }
    gpu_->factory->MakeWindowAssociation(t.hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    t.swapchain->SetMaximumFrameLatency(1);
    t.waitable = t.swapchain->GetFrameLatencyWaitableObject();
    ComPtr<ID3D11Texture2D> back;
    t.swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) gpu_->device->CreateRenderTargetView(back.Get(), nullptr, &t.rtv);
    if (!t.rtv || !t.waitable) {
        Log("monitor %d: swapchain setup failed", t.index);
        ReleaseSwapchain(t);
        return false;
    }
    UpdateLowRes(t);
    stats_[t.index].hasSwapchain = true;
    t.hiddenSince = QpcNow();
    return true;
}

void Renderer::ReleaseSwapchain(Target& t) {
    if (gpu_) {
        gpu_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* none = nullptr;
        gpu_->ctx->PSSetShaderResources(0, 1, &none);
    }
    t.lowSrv.Reset();
    t.lowRtv.Reset();
    t.lowTex.Reset();
    t.rtv.Reset();
    if (t.waitable) CloseHandle(t.waitable);
    t.waitable = nullptr;
    bool had = t.swapchain != nullptr;
    t.swapchain.Reset();
    if (had && gpu_) gpu_->ctx->Flush();  // D3D11 frees released resources at the next flush
    t.wait = Target::Wait::None;
    t.oneShot = false;
    stats_[t.index].hasSwapchain = false;
}

void Renderer::UpdateLowRes(Target& t) {
    int rw = t.width, rh = t.height;
    if (t.scale < 0.999f) {
        rw = std::max(1, int(std::lround(t.width * t.scale)));
        rh = std::max(1, int(std::lround(t.height * t.scale)));
    }
    bool want = t.swapchain && gpu_ && (rw != t.width || rh != t.height);
    if (t.lowTex) {
        D3D11_TEXTURE2D_DESC cur;
        t.lowTex->GetDesc(&cur);
        if (!want || int(cur.Width) != rw || int(cur.Height) != rh) {
            t.lowTex.Reset();
            t.lowRtv.Reset();
            t.lowSrv.Reset();
        }
    }
    if (want && !t.lowTex) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = UINT(rw);
        td.Height = UINT(rh);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(gpu_->device->CreateTexture2D(&td, nullptr, &t.lowTex))) {
            gpu_->device->CreateRenderTargetView(t.lowTex.Get(), nullptr, &t.lowRtv);
            gpu_->device->CreateShaderResourceView(t.lowTex.Get(), nullptr, &t.lowSrv);
        }
        if (!t.lowRtv || !t.lowSrv) {
            Log("monitor %d: cannot create the %dx%d render target, using native size", t.index, rw, rh);
            t.lowTex.Reset();
            t.lowRtv.Reset();
            t.lowSrv.Reset();
        }
    }
    if (!t.lowTex) {
        rw = t.width;
        rh = t.height;
    }
    t.rw = rw;
    t.rh = rh;
    stats_[t.index].renderWidth = rw;
    stats_[t.index].renderHeight = rh;
}

void Renderer::CreatePixelShader(Target& t) {
    const RenderShader& s = shaders_[t.index];
    t.ps.Reset();
    t.mouse = s.mouse;
    float scale = std::clamp(s.renderScale, 0.1f, 1.0f);
    if (scale != t.scale) {
        t.scale = scale;
        UpdateLowRes(t);
    }
    if (!gpu_ || !s.bytecode || s.bytecode->empty()) return;
    HRESULT hr = gpu_->device->CreatePixelShader(s.bytecode->data(), s.bytecode->size(), nullptr, &t.ps);
    if (FAILED(hr)) {
        Log("monitor %d: CreatePixelShader failed (0x%08lX)", t.index, static_cast<unsigned long>(hr));
        if (IsDeviceLost(hr)) deviceLost_ = true;
    }
}

void Renderer::ConfigurePacing(Target& t) {
    int fps = std::clamp(t.fps, 1, t.hz);
    // If the refresh rate is a small multiple of the fps, present every n-th vblank and let
    // the latency waitable pace us. Otherwise use the timer.
    double ratio = double(t.hz) / double(fps);
    int n = int(std::lround(ratio));
    t.vsyncPaced = !t.vsyncUnreliable && n >= 1 && n <= 4 && std::fabs(ratio - n) <= 0.05 * n;
    t.syncInterval = t.vsyncPaced ? UINT(n) : 1;
    t.period = QpcFreq() / fps;
    t.paceStart = 0;
}

bool Renderer::VsyncPacingHolds(Target& t, int64_t now) {
    // Presents complete on DWM's composition clock, which with mixed refresh rates can run
    // faster than this monitor (seen: a 60 Hz panel paced at a 200 Hz primary's rate).
    if (t.paceStart == 0) {
        t.paceStart = now;
        t.paceFrames = 0;
        return true;
    }
    ++t.paceFrames;
    int64_t elapsed = now - t.paceStart;
    if (elapsed < QpcFreq()) return true;
    double achieved = t.paceFrames / QpcToSec(elapsed);
    t.paceStart = now;
    t.paceFrames = 0;
    if (achieved <= 1.2 * std::clamp(t.fps, 1, t.hz)) return true;
    Log("monitor %d: vsync pacing gave %.0f fps for a cap of %d; using the timer", t.index, achieved, t.fps);
    t.vsyncUnreliable = true;
    ConfigurePacing(t);
    return false;
}

void Renderer::ApplyRunState(Target& t) {
    const RunState& rs = run_[t.index];
    bool wasPaused = t.paused;
    bool wasHidden = t.paused && t.hidden;
    t.paused = rs.paused;
    t.fps = rs.fps;
    t.hidden = rs.hidden;
    if (t.paused) {
        if (t.hidden && !wasHidden) t.hiddenSince = QpcNow();
        if (!t.hidden && !t.swapchain && CreateSwapchain(t)) {
            RequestFrame(t);  // released while hidden and visible again: put the frozen frame back
            return;
        }
        if (!t.oneShot) t.wait = Target::Wait::None;
        return;
    }
    if (!t.swapchain) CreateSwapchain(t);  // released during a long pause
    ConfigurePacing(t);
    if (!t.ps || !t.swapchain) {
        t.wait = Target::Wait::None;
        return;
    }
    if (wasPaused || t.wait == Target::Wait::None) {
        t.lastFrame = 0;  // iTime resumes where it stopped
        t.wait = Target::Wait::Swap;
    } else if (t.wait == Target::Wait::Timer) {
        if (t.vsyncPaced)
            t.wait = Target::Wait::Swap;
        else
            t.nextDue = std::min(t.nextDue, QpcNow() + t.period);
    }
}

void Renderer::RequestFrame(Target& t) {
    if (!t.ps || !t.swapchain) return;
    t.oneShot = true;
    t.wait = Target::Wait::Swap;
}

void Renderer::RenderFrame(Target& t) {
    if (!gpu_ || !t.ps || !t.swapchain) {
        t.wait = Target::Wait::None;
        t.oneShot = false;
        return;
    }
    int64_t now = QpcNow();
    double dt = 0.0;
    if (!t.paused) {
        dt = t.lastFrame ? QpcToSec(now - t.lastFrame) : QpcToSec(t.period);
        t.time += dt;
        // iTime is a float: wrap so its step stays below 0.5 ms (one visible jump every 2 h).
        if (t.time >= 7200.0) t.time -= 7200.0;
        t.lastFrame = now;
    }

    GpuGlobals g{};
    g.iResolution[0] = float(t.rw);
    g.iResolution[1] = float(t.rh);
    g.iResolution[2] = 1.0f;
    g.iTime = float(t.time);
    g.iTimeDelta = float(dt);
    g.iFrame = t.frame;
    for (auto& c : g.iClick) c[2] = kNoClickAge;
    if (t.mouse) {
        POINT p;
        if (GetCursorPos(&p)) {  // fails on the secure desktop: keep the last position
            bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            t.input.Update(p, down, t.screen, now);
        }
        if (t.input.lastQpc) t.input.Fill(g, t.screen, t.rw, t.rh, now);
    }
    double seconds = globals_.secondsOfDay;
    if (globals_.refQpc) seconds = std::fmod(seconds + QpcToSec(now - globals_.refQpc), 86400.0);
    g.iDate[0] = globals_.year;
    g.iDate[1] = globals_.month;
    g.iDate[2] = globals_.day;
    g.iDate[3] = float(seconds);
    g.iBattery = globals_.battery;
    g.iOnBattery = globals_.onBattery;
    g.iCpuLoad = globals_.cpuLoad;
    g.iMonitor = float(t.index);

    ID3D11DeviceContext* ctx = gpu_->ctx.Get();
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(gpu_->cbuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, &g, sizeof(g));
        ctx->Unmap(gpu_->cbuffer.Get(), 0);
    }

    D3D11_VIEWPORT vp{0.0f, 0.0f, float(t.rw), float(t.rh), 0.0f, 1.0f};
    ctx->PSSetShader(t.ps.Get(), nullptr, 0);
    if (t.lowRtv) {
        ctx->OMSetRenderTargets(1, t.lowRtv.GetAddressOf(), nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
        vp.Width = float(t.width);
        vp.Height = float(t.height);
        ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(gpu_->blit.Get(), nullptr, 0);
        ctx->PSSetShaderResources(0, 1, t.lowSrv.GetAddressOf());
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    } else {
        ctx->OMSetRenderTargets(1, t.rtv.GetAddressOf(), nullptr);
        ctx->RSSetViewports(1, &vp);
        ctx->Draw(3, 0);
    }

    HRESULT hr = t.swapchain->Present(t.syncInterval, 0);
    if (IsDeviceLost(hr)) {
        deviceLost_ = true;
        return;
    }
    t.oneShot = false;
    if (FAILED(hr)) {
        Log("monitor %d: Present failed (0x%08lX)", t.index, static_cast<unsigned long>(hr));
        t.wait = Target::Wait::None;
        return;
    }
    ++t.frame;
    stats_[t.index].frames.fetch_add(1, std::memory_order_relaxed);

    if (t.paused) {
        t.wait = Target::Wait::None;
        t.hiddenSince = now;
    } else if (t.vsyncPaced && VsyncPacingHolds(t, now)) {
        t.wait = Target::Wait::Swap;
    } else {
        // The next multiple of the period on a grid shared by all monitors: no catch-up bursts
        // after a stall, and monitors with related rates share wake-ups. (Measured: DWM's cost
        // per present is the same aligned or not; it composes each monitor separately.) The
        // 1 ms slack keeps a frame rendered slightly early from landing on its own slot.
        int64_t slack = QpcFreq() / 1000;
        t.nextDue = epoch_ + ((now - epoch_ + slack) / t.period + 1) * t.period;
        t.wait = Target::Wait::Timer;
    }
}

void Renderer::DestroyTargets() {
    destroying_ = true;
    for (auto& t : targets_) {
        ReleaseSwapchain(*t);
        if (t->hwnd) DestroyWindow(t->hwnd);
        stats_[t->index].width = stats_[t->index].height = 0;
    }
    targets_.clear();
    destroying_ = false;
}

void Renderer::LoseTarget(Target& t) {
    Log("monitor %d: wallpaper window was destroyed (Explorer restarted?)", t.index);
    ReleaseSwapchain(t);
    t.hwnd = nullptr;
    PostMessageW(notify_, notifyMsg_, kTargetLost, t.index);
}

LRESULT CALLBACK Renderer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_DESTROY: {
        auto* self = reinterpret_cast<Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && !self->destroying_) {
            for (auto& t : self->targets_) {
                if (t->hwnd == hwnd) {
                    self->LoseTarget(*t);
                    break;
                }
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
