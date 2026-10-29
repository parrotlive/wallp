#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

// ---- strings ----
std::string Utf8(std::wstring_view w);
std::wstring Wide(std::string_view s);
std::wstring Lower(std::wstring s);

// ---- paths / files ----
std::wstring ExePath();
std::wstring ExeDir();                                   // no trailing backslash
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring FullPath(const std::wstring& path);         // absolute, '/' -> '\'
bool IsAbsolutePath(const std::wstring& path);
std::wstring ResolvePath(const std::wstring& base, const std::wstring& path);  // relative paths are based on `base`
std::wstring FileNameOf(const std::wstring& path);
std::wstring DirOf(const std::wstring& path);
std::wstring StripExtension(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::string& out);
// Writes to a temp file and renames it over `path`.
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size);

// ---- time ----
int64_t QpcNow();
int64_t QpcFreq();
inline double QpcToSec(int64_t ticks) { return double(ticks) / double(QpcFreq()); }
inline int64_t SecToQpc(double sec) { return int64_t(sec * double(QpcFreq())); }

// ---- hashing ----
uint64_t Fnv1a64(const void* data, size_t size, uint64_t h = 0xcbf29ce484222325ull);

// ---- logging (UTF-8, thread-safe) ----
void LogOpen(const std::wstring& path);
void Log(const char* fmt, ...);
