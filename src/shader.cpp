#include "shader.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "util.h"
#include "yyjson.h"

using Microsoft::WRL::ComPtr;

// Bump when the wrapper below changes in a way the hash would not see.
static const uint32_t kCacheVersion = 1;

static const char kPreamble[] = R"(// ---- wallp preamble ----
cbuffer Globals : register(b0) {
    float3 iResolution;   // render-target size in pixels (z = 1)
    float  iTime;         // seconds; frozen while paused
    float  iTimeDelta;
    int    iFrame;
    float4 iMouse;        // xy = cursor, zw = last click (Shadertoy sign rules); needs "mouse": true
    float4 iDate;         // year, month (0-11), day (1-31), seconds of day
    float  iBattery;      // 0..1, -1 if there is no battery
    float  iOnBattery;    // 0 or 1
    float  iCpuLoad;      // 0..1
    float  iMonitor;      // monitor index
    // The rest also needs "mouse": true. Positions are in pixels like fragCoord.
    float2 iMouseVel;     // cursor velocity in pixels per second, smoothed over ~60 ms
    float4 iClick[4];     // last left clicks, newest first: xy = position, z = age in seconds
                          // (10000 = no click yet), w = 1 while the button is still held
    float4 iTrail[16];    // cursor positions ~1/30 s apart, newest first: xy, z = age in seconds
};
#define WALLP 1
// GLSL-style modulo (result takes the sign of y), handy when porting from Shadertoy.
float  mod(float  x, float  y) { return x - y * floor(x / y); }
float2 mod(float2 x, float2 y) { return x - y * floor(x / y); }
float3 mod(float3 x, float3 y) { return x - y * floor(x / y); }
float4 mod(float4 x, float4 y) { return x - y * floor(x / y); }
float2 mod(float2 x, float  y) { return x - y * floor(x / y); }
float3 mod(float3 x, float  y) { return x - y * floor(x / y); }
float4 mod(float4 x, float  y) { return x - y * floor(x / y); }
)";

static const char kEpilogue[] = R"(
struct WallpVSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 PSMain(WallpVSOut i) : SV_Target {
    float4 col = float4(0.0, 0.0, 0.0, 1.0);
    mainImage(col, float2(i.pos.x, iResolution.y - i.pos.y));  // Shadertoy: origin bottom-left
    return float4(col.rgb, 1.0);
}
)";

static std::string BlobText(ID3DBlob* blob) {
    if (!blob) return {};
    const char* p = static_cast<const char*>(blob->GetBufferPointer());
    std::string s(p, strnlen(p, blob->GetBufferSize()));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

static ShaderSettings ReadSidecar(const std::wstring& path) {
    ShaderSettings s;
    std::string text;
    if (!ReadFileBytes(path, text)) return s;
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(text.data(), text.size(),
                                       YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS |
                                           YYJSON_READ_ALLOW_BOM,
                                       nullptr, &err);
    if (!doc) {
        Log("sidecar %s: %s at byte %zu (ignored)", Utf8(path).c_str(), err.msg ? err.msg : "?", err.pos);
        return s;
    }
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (yyjson_val* v = yyjson_obj_get(root, "maxFps"); yyjson_is_num(v))
        s.maxFps = std::clamp(int(yyjson_get_num(v)), 1, 1000);
    if (yyjson_val* v = yyjson_obj_get(root, "renderScale"); yyjson_is_num(v))
        s.renderScale = std::clamp(float(yyjson_get_num(v)), 0.1f, 1.0f);
    if (yyjson_val* v = yyjson_obj_get(root, "mouse"); yyjson_is_bool(v)) s.mouse = yyjson_get_bool(v);
    yyjson_doc_free(doc);
    return s;
}

void ShaderCompiler::Start(HWND notify, UINT msg, const std::wstring& cacheDir, const char* profile) {
    notify_ = notify;
    msg_ = msg;
    cacheDir_ = cacheDir;
    profile_ = profile;
    stop_ = false;
    thread_ = std::thread([this] { ThreadMain(); });
}

void ShaderCompiler::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_one();
    thread_.join();
}

void ShaderCompiler::Request(const std::wstring& path) {
    {
        std::lock_guard lock(mutex_);
        if (std::find(queue_.begin(), queue_.end(), path) != queue_.end()) return;
        queue_.push_back(path);
    }
    cv_.notify_one();
}

void ShaderCompiler::ThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    PruneCache();
    for (;;) {
        std::wstring path;
        {
            std::unique_lock lock(mutex_);
            auto ready = [this] { return stop_ || !queue_.empty(); };
            if (!cv_.wait_for(lock, std::chrono::seconds(10), ready)) {
                // Quiet for a while: give the compiler DLL's memory back.
                lock.unlock();
                UnloadCompiler();
                lock.lock();
                cv_.wait(lock, ready);
            }
            if (stop_) break;
            path = std::move(queue_.front());
            queue_.pop_front();
        }
        auto* result = new CompiledShader(Compile(path));
        if (!PostMessageW(notify_, msg_, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }
    UnloadCompiler();
}

bool ShaderCompiler::LoadCompiler() {
    if (module_) return true;
    module_ = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) return false;
    compile_ = GetProcAddress(module_, "D3DCompile");
    preprocess_ = GetProcAddress(module_, "D3DPreprocess");
    if (!compile_ || !preprocess_) {
        UnloadCompiler();
        return false;
    }
    return true;
}

void ShaderCompiler::UnloadCompiler() {
    if (module_) FreeLibrary(module_);
    module_ = nullptr;
    compile_ = preprocess_ = nullptr;
}

void ShaderCompiler::PruneCache() {
    // Keep the 64 most recently used entries (hits refresh the write time).
    struct Item {
        std::wstring name;
        FILETIME time;
    };
    std::vector<Item> items;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(cacheDir_, L"*.cso").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        items.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (items.size() <= 64) return;
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return CompareFileTime(&a.time, &b.time) > 0; });
    for (size_t i = 64; i < items.size(); ++i) DeleteFileW(JoinPath(cacheDir_, items[i].name).c_str());
}

static void TouchFile(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    SetFileTime(f, nullptr, nullptr, &now);
    CloseHandle(f);
}

CompiledShader ShaderCompiler::Compile(const std::wstring& path) {
    CompiledShader r;
    r.path = path;
    int64_t t0 = QpcNow();

    std::string src;
    if (!ReadFileBytes(path, src)) {
        r.error = "cannot read " + Utf8(path);
        return r;
    }
    if (src.size() >= 3 && memcmp(src.data(), "\xEF\xBB\xBF", 3) == 0) src.erase(0, 3);
    r.settings = ReadSidecar(StripExtension(path) + L".json");

    if (!LoadCompiler()) {
        r.error = "d3dcompiler_47.dll is not available";
        return r;
    }
    auto preprocess = reinterpret_cast<pD3DPreprocess>(preprocess_);
    auto compile = reinterpret_cast<pD3DCompile>(compile_);

    // Wrap: preamble + user source (with #line so errors point into the user's file) + PSMain.
    std::string pathUtf8 = Utf8(path);
    std::string lineName = pathUtf8;
    std::replace(lineName.begin(), lineName.end(), '\\', '/');
    std::string wrapped;
    wrapped.reserve(sizeof(kPreamble) + src.size() + sizeof(kEpilogue) + lineName.size() + 64);
    wrapped += kPreamble;
    wrapped += "#line 1 \"" + lineName + "\"\n";
    wrapped += src;
    wrapped += "\n#line 1 \"wallp-epilogue\"\n";
    wrapped += kEpilogue;

    ComPtr<ID3DBlob> pre, errors;
    HRESULT hr = preprocess(wrapped.data(), wrapped.size(), pathUtf8.c_str(), nullptr,
                            D3D_COMPILE_STANDARD_FILE_INCLUDE, &pre, &errors);
    if (FAILED(hr) || !pre) {
        r.error = BlobText(errors.Get());
        if (r.error.empty()) r.error = "preprocessing failed";
        return r;
    }
    const char* preText = static_cast<const char*>(pre->GetBufferPointer());
    size_t preLen = strnlen(preText, pre->GetBufferSize());

    // The cache key covers everything that reaches the compiler, includes included.
    uint64_t h = Fnv1a64(preText, preLen);
    h = Fnv1a64(profile_.data(), profile_.size(), h);
    h = Fnv1a64(&kCacheVersion, sizeof(kCacheVersion), h);
    r.hash = h;
    wchar_t name[32];
    swprintf_s(name, L"%016llx.cso", static_cast<unsigned long long>(h));
    std::wstring cacheFile = JoinPath(cacheDir_, name);

    std::string cached;
    if (ReadFileBytes(cacheFile, cached) && cached.size() > 4 && memcmp(cached.data(), "DXBC", 4) == 0) {
        r.bytecode = std::make_shared<std::vector<uint8_t>>(cached.begin(), cached.end());
        r.fromCache = true;
        r.ok = true;
        TouchFile(cacheFile);
    } else {
        ComPtr<ID3DBlob> code;
        errors.Reset();
        hr = compile(preText, preLen, pathUtf8.c_str(), nullptr, nullptr, "PSMain", profile_.c_str(),
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr) || !code) {
            r.error = BlobText(errors.Get());
            if (r.error.empty()) {
                char buf[64];
                snprintf(buf, sizeof(buf), "compilation failed (0x%08lX)", static_cast<unsigned long>(hr));
                r.error = buf;
            }
            return r;
        }
        if (errors) Log("shader %s warnings:\n%s", pathUtf8.c_str(), BlobText(errors.Get()).c_str());
        auto bytes = static_cast<const uint8_t*>(code->GetBufferPointer());
        r.bytecode = std::make_shared<std::vector<uint8_t>>(bytes, bytes + code->GetBufferSize());
        r.ok = true;
        EnsureDir(cacheDir_);
        if (!WriteFileAtomic(cacheFile, bytes, code->GetBufferSize()))
            Log("shader cache: cannot write %s", Utf8(cacheFile).c_str());
    }
    r.ms = QpcToSec(QpcNow() - t0) * 1000.0;
    return r;
}
