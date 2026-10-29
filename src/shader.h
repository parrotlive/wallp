#pragma once
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Optional sidecar <shader>.json.
struct ShaderSettings {
    int maxFps = 0;            // 0 = no cap of its own
    float renderScale = 0.0f;  // 0 = not set
    bool mouse = false;        // poll the cursor for iMouse

    bool operator==(const ShaderSettings&) const = default;
};

struct CompiledShader {
    std::wstring path;  // normalized full path
    bool ok = false;
    bool fromCache = false;
    std::string error;
    std::shared_ptr<const std::vector<uint8_t>> bytecode;
    uint64_t hash = 0;  // of the preprocessed source + profile
    ShaderSettings settings;
    double ms = 0.0;
};

// One worker thread that wraps user shaders in the Shadertoy preamble, compiles them with
// D3DCompile and caches the bytecode on disk. Each result is posted to `notify` as `msg`
// with lParam = CompiledShader* (the receiver deletes it). d3dcompiler_47.dll is loaded on
// demand and unloaded again after a quiet period.
class ShaderCompiler {
public:
    ~ShaderCompiler() { Stop(); }
    void Start(HWND notify, UINT msg, const std::wstring& cacheDir, const char* profile);
    void Stop();
    void Request(const std::wstring& path);

private:
    void ThreadMain();
    CompiledShader Compile(const std::wstring& path);
    bool LoadCompiler();
    void UnloadCompiler();
    void PruneCache();

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::wstring cacheDir_;
    std::string profile_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::wstring> queue_;
    bool stop_ = false;
    HMODULE module_ = nullptr;
    FARPROC compile_ = nullptr;
    FARPROC preprocess_ = nullptr;
};
