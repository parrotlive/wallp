#include "policy.h"

#include <algorithm>

const char* ReasonName(PauseReason r) {
    switch (r) {
    case PauseReason::None: return "running";
    case PauseReason::User: return "paused by user";
    case PauseReason::Sleep: return "sleep";
    case PauseReason::Locked: return "locked";
    case PauseReason::DisplayOff: return "display off";
    case PauseReason::Inactive: return "user inactive";
    case PauseReason::Fullscreen: return "fullscreen app";
    case PauseReason::Covered: return "covered";
    }
    return "?";
}

const char* TierName(Tier t) {
    switch (t) {
    case Tier::Normal: return "normal";
    case Tier::Busy: return "busy";
    case Tier::Game: return "game";
    case Tier::Forced: return "forced";
    }
    return "?";
}

bool Policy::Debounce(Hold& h, bool raw, int64_t nowMs, int64_t& recheckMs, bool settled) {
    if (raw != h.raw) {
        h.raw = raw;
        h.since = nowMs;
    }
    if (!raw) {
        h.effective = false;
    } else if (!h.effective) {
        int64_t held = nowMs - h.since;
        if (settled || held >= kHoldMs) {
            h.effective = true;
        } else {
            int64_t left = kHoldMs - held;
            recheckMs = recheckMs < 0 ? left : std::min(recheckMs, left);
        }
    }
    return h.effective;
}

std::vector<MonitorDecision> Policy::Evaluate(const PolicyInput& in, const Config& cfg, int64_t nowMs,
                                              int64_t& recheckMs) {
    recheckMs = -1;
    const size_t n = in.monitors.size();
    if (n == 0) return {};  // not attached yet (e.g. a power notification during startup)
    fullscreen_.resize(n);
    covered_.resize(n);

    Tier tier = Tier::Normal;
    int tierFps = cfg.fpsIdle;
    if (in.forcedFps > 0) {
        tier = Tier::Forced;
        tierFps = in.forcedFps;
    } else if (in.game) {
        tier = Tier::Game;
        tierFps = cfg.fpsGame;
    } else if (in.busy) {
        tier = Tier::Busy;
        tierFps = cfg.fpsBusy;
    }

    bool global = in.userPaused || in.suspended || in.locked || in.displayOff || (in.userIdle && !in.noAutoPause);
    bool settled = wasGlobal_ && !global;
    wasGlobal_ = global;

    std::vector<MonitorDecision> out(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& m = in.monitors[i];
        bool fullscreen = Debounce(fullscreen_[i], m.fullscreen && !in.noAutoPause, nowMs, recheckMs, settled);
        bool covered = Debounce(covered_[i], m.covered && !in.noAutoPause, nowMs, recheckMs, settled);

        PauseReason r = PauseReason::None;
        if (in.userPaused) r = PauseReason::User;
        else if (in.suspended) r = PauseReason::Sleep;
        else if (in.locked) r = PauseReason::Locked;
        else if (in.displayOff) r = PauseReason::DisplayOff;
        else if (in.userIdle && !in.noAutoPause) r = PauseReason::Inactive;
        else if (fullscreen) r = PauseReason::Fullscreen;
        else if (covered) r = PauseReason::Covered;

        MonitorDecision& d = out[i];
        d.paused = r != PauseReason::None;
        d.reason = r;
        d.tier = tier;
        int fps = tierFps;
        if (in.onBattery && fps > cfg.fpsBattery) {
            fps = cfg.fpsBattery;
            d.batteryCapped = true;
        }
        if (m.shaderMaxFps > 0) fps = std::min(fps, m.shaderMaxFps);  // a sidecar can only lower it
        d.fps = std::clamp(fps, 1, std::max(1, m.hz));
    }
    return out;
}
