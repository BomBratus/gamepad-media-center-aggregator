#pragma once
#include <map>
#include <cstdint>
#include <string>
namespace stremio {
inline bool completedPosition(int64_t position, int64_t duration) {
    return duration > 0 && double(position) / double(duration) >= 0.90;
}

// Movie completion clears account resume; episode completion needs its final
// position to distinguish it from a deliberate restart at zero on another device.
inline int64_t remotePlaybackOffset(bool episode, bool completed, int64_t position) {
    return completed && !episode ? 0 : position;
}

// Caller holds its checkpoint mutex through enqueueing the accepted mutation.
// A source reload shares the session; explicit replay starts a new session.
class PlaybackCompletion {
public:
    void begin(const std::string& key, const std::string& session) {
        auto& state = states_[key];
        if (state.session == session) return;
        state = {session, false};
    }
    bool accept(const std::string& key, bool complete) {
        auto& state = states_[key];
        if (state.complete) return false;
        state.complete = complete;
        return true;
    }
    void unwatch(const std::string& key) { states_[key].complete = false; }
    bool canSave(const std::string& key) const {
        auto state = states_.find(key);
        return state == states_.end() || !state->second.complete;
    }
private:
    struct State { std::string session; bool complete = false; };
    std::map<std::string, State> states_;
};
} // namespace stremio
