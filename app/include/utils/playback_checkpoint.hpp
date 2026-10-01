#pragma once

#include <cstdint>

namespace utils {

/// Tracks whether playback lifecycle events make observed positions safe to
/// persist. Loading a file alone does not make subsequent samples authoritative;
/// a restart event must confirm that the player resumed the requested item.
class PlaybackCheckpoint {
public:
    void begin(int64_t position) {
        position_ = position;
        loaded_ = false;
        ready_ = false;
    }

    void loaded() { loaded_ = true; }

    void suspend() { ready_ = false; }

    void restart() { ready_ = loaded_; }

    /// Accept only nonnegative player samples after a loaded restart.
    bool observe(int64_t position) {
        if (!ready_ || position < 0) return false;
        position_ = position;
        return true;
    }

    /// Invalidate lifecycle readiness while retaining the last safe position.
    void stop() {
        ready_ = false;
        loaded_ = false;
    }

    bool ready() const { return ready_; }
    int64_t position() const { return position_; }

private:
    int64_t position_ = 0;
    bool loaded_ = false;
    bool ready_ = false;
};

}  // namespace utils
