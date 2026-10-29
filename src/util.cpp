#include "util.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>

std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), DWORD(s.size()));
    return s;
}

std::wstring ExePath() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
        if (n == 0) return {};
        if (n < p.size()) {
            p.resize(n);
            return p;
        }
        p.resize(p.size() * 2);
    }
}

std::wstring ExeDir() { return DirOf(ExePath()); }

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring FullPath(const std::wstring& path) {
    std::wstring p = path;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return p;
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, out.data(), nullptr);
    out.resize(n);
    return out;
}

bool IsAbsolutePath(const std::wstring& path) {
    if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/'))
        return true;  // UNC
    return path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

std::wstring ResolvePath(const std::wstring& base, const std::wstring& path) {
    return FullPath(IsAbsolutePath(path) ? path : JoinPath(base, path));
}

static size_t LastSeparator(const std::wstring& path) { return path.find_last_of(L"\\/"); }

std::wstring FileNameOf(const std::wstring& path) {
    size_t i = LastSeparator(path);
    return i == std::wstring::npos ? path : path.substr(i + 1);
}

std::wstring DirOf(const std::wstring& path) {
    size_t i = LastSeparator(path);
    return i == std::wstring::npos ? std::wstring() : path.substr(0, i);
}

std::wstring StripExtension(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    size_t sep = LastSeparator(path);
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return path;
    return path.substr(0, dot);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDir(const std::wstring& path) {
    if (DirExists(path)) return true;
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (64ll << 20);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(f, out.data(), DWORD(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, data, DWORD(size), &written, nullptr) && written == size;
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

int64_t QpcFreq() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

uint64_t Fnv1a64(const void* data, size_t size, uint64_t h) {
    auto p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static std::mutex g_logMutex;
static HANDLE g_log = INVALID_HANDLE_VALUE;

void LogOpen(const std::wstring& path) {
    // Keep appending across runs, but start over once the file gets large.
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    DWORD disposition = OPEN_ALWAYS;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
        (fa.nFileSizeHigh != 0 || fa.nFileSizeLow > (1u << 20)))
        disposition = CREATE_ALWAYS;
    std::lock_guard lock(g_logMutex);
    g_log = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Log(const char* fmt, ...) {
    char msg[2048];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = snprintf(msg, sizeof(msg), "%04u-%02u-%02u %02u:%02u:%02u.%03u  ", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int avail = int(sizeof(msg)) - n - 3;
    int m = vsnprintf(msg + n, size_t(avail), fmt, ap);
    va_end(ap);
    size_t len = size_t(n) + size_t(std::clamp(m, 0, avail - 1));
    msg[len++] = '\r';
    msg[len++] = '\n';
    msg[len] = '\0';
    std::lock_guard lock(g_logMutex);
    if (g_log != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_log, msg, DWORD(len), &written, nullptr);
    }
#ifndef NDEBUG
    OutputDebugStringA(msg);
#endif
}
