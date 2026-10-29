#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "util.h"
#include "yyjson.h"

static const char kDefaultConfig[] = R"({
  "monitors": { "0": "shaders/plasma.hlsl", "1": "shaders/stars.hlsl" },
  "fps": { "idle": 120, "busy": 60, "game": 15, "battery": 30 },
  "renderScale": 1.0,
  "idleTimeoutSec": 600,
  "gpuGameThreshold": 0.20,
  "games": ["eldenring.exe"],
  "ignore": ["obs64.exe"],
  "video": ["chrome.exe", "msedge.exe", "firefox.exe", "brave.exe", "opera.exe", "vlc.exe", "mpv.exe",
            "mpc-hc64.exe", "mpc-be64.exe", "potplayermini64.exe", "wmplayer.exe", "video.ui.exe"],
  "autostart": false,
  "showStats": true,
  "theme": "system",
  "releaseAfterPauseSec": 300
}
)";

static const yyjson_read_flag kReadFlags =
    YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS | YYJSON_READ_ALLOW_BOM;

static yyjson_doc* ParseJson(std::string& text, std::string& error) {
    yyjson_read_err err{};
    yyjson_doc* doc = yyjson_read_opts(text.data(), text.size(), kReadFlags, nullptr, &err);
    if (!doc) {
        size_t line = 0, col = 0, chr = 0;
        yyjson_locate_pos(text.data(), text.size(), err.pos, &line, &col, &chr);
        char buf[256];
        snprintf(buf, sizeof(buf), "line %zu, column %zu: %s", line, col, err.msg ? err.msg : "parse error");
        error = buf;
    }
    return doc;
}

static void ReadInt(yyjson_val* obj, const char* key, int& out, int lo, int hi) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (yyjson_is_num(v)) out = std::clamp(int(yyjson_get_num(v)), lo, hi);
}

static void ReadFloat(yyjson_val* obj, const char* key, float& out, float lo, float hi) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (yyjson_is_num(v)) out = std::clamp(float(yyjson_get_num(v)), lo, hi);
}

static void ReadBool(yyjson_val* obj, const char* key, bool& out) {
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (yyjson_is_bool(v)) out = yyjson_get_bool(v);
}

static void ReadExeList(yyjson_val* obj, const char* key, std::vector<std::wstring>& out) {
    yyjson_val* arr = yyjson_obj_get(obj, key);
    if (!yyjson_is_arr(arr)) return;
    out.clear();
    size_t idx, max;
    yyjson_val* v;
    yyjson_arr_foreach(arr, idx, max, v) {
        if (!yyjson_is_str(v)) continue;
        std::wstring name = Lower(Wide(std::string_view(yyjson_get_str(v), yyjson_get_len(v))));
        if (name.empty()) continue;
        if (name.find(L'.') == std::wstring::npos) name += L".exe";
        out.push_back(std::move(name));
    }
}

static bool ParseInto(std::string text, Config& c, std::string& error) {
    yyjson_doc* doc = ParseJson(text, error);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        error = "the top level must be a JSON object";
        yyjson_doc_free(doc);
        return false;
    }

    if (yyjson_val* mons = yyjson_obj_get(root, "monitors"); yyjson_is_obj(mons)) {
        c.monitors.clear();
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(mons, idx, max, key, val) {
            char* end = nullptr;
            long index = strtol(yyjson_get_str(key), &end, 10);
            if (!yyjson_is_str(val) || end == yyjson_get_str(key) || *end || index < 0 || index > 63) {
                Log("config: ignoring monitors entry \"%s\"", yyjson_get_str(key));
                continue;
            }
            c.monitors[int(index)] = Wide(std::string_view(yyjson_get_str(val), yyjson_get_len(val)));
        }
    }
    if (yyjson_val* fps = yyjson_obj_get(root, "fps"); yyjson_is_obj(fps)) {
        ReadInt(fps, "idle", c.fpsIdle, 1, 1000);
        ReadInt(fps, "busy", c.fpsBusy, 1, 1000);
        ReadInt(fps, "game", c.fpsGame, 1, 1000);
        ReadInt(fps, "battery", c.fpsBattery, 1, 1000);
    }
    ReadFloat(root, "renderScale", c.renderScale, 0.1f, 1.0f);
    ReadInt(root, "idleTimeoutSec", c.idleTimeoutSec, 0, 7 * 24 * 3600);
    ReadFloat(root, "gpuGameThreshold", c.gpuGameThreshold, 0.0f, 100.0f);
    if (c.gpuGameThreshold > 1.0f) c.gpuGameThreshold /= 100.0f;  // accept "20" as 20%
    ReadExeList(root, "games", c.games);
    ReadExeList(root, "ignore", c.ignore);
    ReadExeList(root, "video", c.video);
    ReadBool(root, "autostart", c.autostart);
    ReadBool(root, "showStats", c.showStats);
    if (yyjson_val* v = yyjson_obj_get(root, "theme"); yyjson_is_str(v)) {
        std::string t = yyjson_get_str(v);
        if (t == "system") c.theme = 0;
        else if (t == "light") c.theme = 1;
        else if (t == "dark") c.theme = 2;
        else Log("config: theme must be \"system\", \"light\" or \"dark\"");
    }
    ReadInt(root, "releaseAfterPauseSec", c.releaseAfterPauseSec, 0, 7 * 24 * 3600);

    yyjson_doc_free(doc);
    return true;
}

Config DefaultConfig() {
    Config c;
    std::string error;
    ParseInto(kDefaultConfig, c, error);
    return c;
}

bool ParseConfig(std::string text, Config& cfg, std::string& error) {
    Config c = DefaultConfig();
    if (!ParseInto(std::move(text), c, error)) return false;
    cfg = std::move(c);
    return true;
}

bool LoadConfig(const std::wstring& path, Config& cfg, std::string& error) {
    std::string text;
    if (!ReadFileBytes(path, text)) {
        error = "cannot read " + Utf8(path);
        return false;
    }
    return ParseConfig(std::move(text), cfg, error);
}

bool WriteDefaultConfig(const std::wstring& path) {
    return WriteFileAtomic(path, kDefaultConfig, sizeof(kDefaultConfig) - 1);
}

bool SaveConfig(const std::wstring& path, const Config& cfg, std::string& error) {
    yyjson_mut_doc* md = nullptr;
    std::string text;
    if (ReadFileBytes(path, text)) {
        std::string parseError;
        yyjson_doc* doc = ParseJson(text, parseError);
        if (!doc) {
            error = "config.json has an error (" + parseError + "); fix it before changing settings from the tray";
            return false;
        }
        md = yyjson_doc_mut_copy(doc, nullptr);
        yyjson_doc_free(doc);
    } else {
        md = yyjson_mut_doc_new(nullptr);
    }
    if (!md) {
        error = "out of memory";
        return false;
    }
    yyjson_mut_val* root = yyjson_mut_doc_get_root(md);
    if (!yyjson_mut_is_obj(root)) {
        root = yyjson_mut_obj(md);
        yyjson_mut_doc_set_root(md, root);
    }

    yyjson_mut_val* mons = yyjson_mut_obj(md);
    for (const auto& [index, shader] : cfg.monitors) {
        std::string key = std::to_string(index);
        std::string val = Utf8(shader);
        yyjson_mut_obj_add(mons, yyjson_mut_strncpy(md, key.data(), key.size()),
                           yyjson_mut_strncpy(md, val.data(), val.size()));
    }
    yyjson_mut_obj_put(root, yyjson_mut_str(md, "monitors"), mons);
    yyjson_mut_obj_put(root, yyjson_mut_str(md, "autostart"), yyjson_mut_bool(md, cfg.autostart));
    yyjson_mut_obj_put(root, yyjson_mut_str(md, "showStats"), yyjson_mut_bool(md, cfg.showStats));
    static const char* const kThemes[] = {"system", "light", "dark"};
    yyjson_mut_obj_put(root, yyjson_mut_str(md, "theme"), yyjson_mut_str(md, kThemes[cfg.theme]));

    size_t len = 0;
    char* out = yyjson_mut_write_opts(md, YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END, nullptr,
                                      &len, nullptr);
    yyjson_mut_doc_free(md);
    if (!out) {
        error = "failed to serialize config";
        return false;
    }
    bool ok = WriteFileAtomic(path, out, len);
    free(out);
    if (!ok) error = "cannot write " + Utf8(path);
    return ok;
}

// ---- autostart ----

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[] = L"wallp";

bool SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    LSTATUS st;
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\"";
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool IsAutostartEnabled() {
    wchar_t buf[2048];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    std::wstring want = L"\"" + ExePath() + L"\"";
    return _wcsicmp(buf, want.c_str()) == 0;
}

// ---- DirWatcher ----

bool DirWatcher::Arm(Entry& e) {
    e.pending = ReadDirectoryChangesW(e.handle, e.buffer, sizeof(e.buffer), e.subtree,
                                      FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE, nullptr, &e.ov,
                                      nullptr) != 0;
    return e.pending;
}

bool DirWatcher::Watch(const std::wstring& dir, bool subtree, int tag) {
    auto e = std::make_unique<Entry>();
    e->dir = dir;
    e->tag = tag;
    e->subtree = subtree;
    e->handle = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (e->handle == INVALID_HANDLE_VALUE) return false;
    e->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!e->ov.hEvent || !Arm(*e)) {
        if (e->ov.hEvent) CloseHandle(e->ov.hEvent);
        CloseHandle(e->handle);
        return false;
    }
    entries_.push_back(std::move(e));
    return true;
}

void DirWatcher::Clear() {
    for (auto& e : entries_) {
        if (e->pending) {
            CancelIoEx(e->handle, &e->ov);
            DWORD bytes = 0;
            GetOverlappedResult(e->handle, &e->ov, &bytes, TRUE);  // the buffer must outlive the I/O
        }
        CloseHandle(e->handle);
        CloseHandle(e->ov.hEvent);
    }
    entries_.clear();
}

void DirWatcher::Collect(size_t i, std::vector<Change>& out) {
    Entry& e = *entries_[i];
    if (!e.pending) {
        ResetEvent(e.ov.hEvent);
        return;
    }
    e.pending = false;
    DWORD bytes = 0;
    if (!GetOverlappedResult(e.handle, &e.ov, &bytes, FALSE)) {
        // The directory went away or the handle broke; stop watching it (it is re-added on reload).
        Log("watcher: %s stopped (error %lu)", Utf8(e.dir).c_str(), GetLastError());
        ResetEvent(e.ov.hEvent);
        out.push_back({e.tag, std::wstring()});
        return;
    }
    if (bytes == 0) {
        out.push_back({e.tag, std::wstring()});  // overflow: something changed, details unknown
    } else {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(e.buffer);
        for (;;) {
            std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
            out.push_back({e.tag, JoinPath(e.dir, name)});
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(reinterpret_cast<const BYTE*>(info) +
                                                                    info->NextEntryOffset);
        }
    }
    if (!Arm(e)) ResetEvent(e.ov.hEvent);
}
