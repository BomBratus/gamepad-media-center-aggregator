#pragma once
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

namespace gmca {
// Mirrors player leases already reported to Archive. It never owns mpv or UI.
class BackgroundGovernor {
public:
    using Clock = std::chrono::steady_clock;
    enum class State { Idle, Opening, Unhealthy, Stable, Stopped };
    explicit BackgroundGovernor(std::chrono::milliseconds grace = std::chrono::seconds(10)) : grace(grace) {}
    void begin(uint64_t id) { std::lock_guard<std::mutex> lock(mutex); players[id] = {State::Opening, {}}; changed.notify_all(); }
    void update(uint64_t id, bool healthy) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = players.find(id);
        if (it == players.end()) return;
        if (!healthy) it->second = {State::Unhealthy, {}};
        else if (it->second.state != State::Stable) it->second = {State::Stable, Clock::now()};
        changed.notify_all();
    }
    void end(uint64_t id) { std::lock_guard<std::mutex> lock(mutex); players.erase(id); changed.notify_all(); }
    State state() const { std::lock_guard<std::mutex> lock(mutex); return stateLocked(); }
    bool backgroundAllowed() const { const auto value = state(); return value == State::Idle || value == State::Stable; }
    void stop() { std::lock_guard<std::mutex> lock(mutex); stopped = true; changed.notify_all(); }
    // Cooperative receive pacing, evaluated on every curl progress tick. Keep
    // active connections alive during opening/buffering with a 64 KiB/s budget;
    // stable playback permits 256 KiB/s. Idle leaves the transfer unrestricted.
    // This is not a hard network or real-time guarantee.
    void transfer(const std::shared_ptr<std::atomic_bool>& cancel, size_t bytes) {
        std::unique_lock<std::mutex> lock(mutex);
        auto value = stateLocked();
        if (value == State::Idle) { deadline = {}; return; }
        if (stopped || cancel->load()) return;
        const auto rate = value == State::Stable ? 256 * 1024 : 64 * 1024;
        deadline = std::max(Clock::now(), deadline) + std::chrono::microseconds(uint64_t(bytes) * 1000000 / rate);
        while (!stopped && !cancel->load() && stateLocked() != State::Idle && Clock::now() < deadline)
            changed.wait_for(lock, std::chrono::milliseconds(100));
    }
private:
    struct Player { State state; Clock::time_point stableSince; };
    State stateLocked() const {
        if (stopped) return State::Stopped;
        if (players.empty()) return State::Idle;
        for (const auto& player : players) {
            if (player.second.state != State::Stable) return player.second.state;
            if (Clock::now() - player.second.stableSince < grace) return State::Opening;
        }
        return State::Stable;
    }
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::map<uint64_t, Player> players;
    std::chrono::milliseconds grace;
    Clock::time_point deadline{};
    bool stopped = false;
};
inline BackgroundGovernor& backgroundGovernor() { static BackgroundGovernor governor; return governor; }
} // namespace gmca
