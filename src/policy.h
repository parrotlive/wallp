#pragma once
#include <cstdint>
#include <vector>

#include "config.h"

enum class PauseReason { None, User, Sleep, Locked, DisplayOff, Inactive, Fullscreen, Covered };
enum class Tier { Normal, Busy, Game, Forced };

const char* ReasonName(PauseReason r);
const char* TierName(Tier t);

struct PolicyInput {
    bool userPaused = false;
    bool suspended = false;
    bool locked = false;
    bool displayOff = false;
    bool userIdle = false;
    bool onBattery = false;
    bool game = false;        // game detected (any monitor)
    bool busy = false;        // video / GPU-busy app detected
    int forcedFps = 0;        // tray "FPS tier", 0 = auto
    bool noAutoPause = false; // --bench: ignore fullscreen / covered / idle

    struct Monitor {
        int hz = 60;
        int shaderMaxFps = 0;  // sidecar cap, 0 = none
        bool fullscreen = false;
        bool covered = false;
    };
    std::vector<Monitor> monitors;
};

struct MonitorDecision {
    bool paused = true;
    PauseReason reason = PauseReason::None;
    int fps = 0;
    Tier tier = Tier::Normal;
    bool batteryCapped = false;

    bool operator==(const MonitorDecision&) const = default;
};

// Pure state machine: inputs in, per-monitor run state out. Fullscreen and covered only
// take effect after holding for kHoldMs (window drags flicker); clearing is immediate, and
// so is the first evaluation at startup or after a global pause (user, idle, lock, ...),
// when no window is in motion.
class Policy {
public:
    static constexpr int64_t kHoldMs = 300;

    // `recheckMs` receives the delay until a pending transition takes effect (-1 if none).
    std::vector<MonitorDecision> Evaluate(const PolicyInput& in, const Config& cfg, int64_t nowMs,
                                          int64_t& recheckMs);

private:
    struct Hold {
        bool raw = false;
        bool effective = false;
        int64_t since = 0;
    };
    static bool Debounce(Hold& h, bool raw, int64_t nowMs, int64_t& recheckMs, bool settled);

    std::vector<Hold> fullscreen_, covered_;
    bool wasGlobal_ = true;  // startup counts as leaving a pause: nothing is moving yet
};
