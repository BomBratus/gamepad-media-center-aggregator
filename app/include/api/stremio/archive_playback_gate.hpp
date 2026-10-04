#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace stremio::archive {

// One staging writer cooperatively yields; published readers never use this
// gate. Player leases protect overlapping views and delayed close callbacks.
class PlaybackGate {
public:
    using Cancel = std::shared_ptr<std::atomic_bool>;
    using Clock = std::chrono::steady_clock;
    enum class Mode { Foreground, Parked, Background, Stopped };
    explicit PlaybackGate(std::chrono::milliseconds grace = std::chrono::seconds(10)) : grace(grace) {}

    Cancel start() {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopped) return {};
        auto cancel = std::make_shared<std::atomic_bool>(false);
        jobs.emplace(cancel, true); // queued work has not touched disk yet
        return cancel;
    }
    void finish(const Cancel& cancel) {
        std::lock_guard<std::mutex> lock(mutex);
        jobs.erase(cancel);
        changed.notify_all();
    }
    uint64_t beginPlayback() {
        std::lock_guard<std::mutex> lock(mutex);
        const auto id = ++nextPlayer;
        players.emplace(id, Clock::time_point{});
        changed.notify_all();
        return id;
    }
    void playbackState(uint64_t id, bool healthy) {
        std::lock_guard<std::mutex> lock(mutex);
        auto player = players.find(id);
        if (player == players.end()) return;
        if (!healthy) player->second = {};
        else if (player->second == Clock::time_point{}) player->second = Clock::now();
        changed.notify_all();
    }
    void endPlayback(uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex);
        players.erase(id);
        changed.notify_all();
    }
    Mode mode() {
        std::lock_guard<std::mutex> lock(mutex);
        return modeLocked();
    }
    // Stream resolution waits only for the writer to park, not for completion.
    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [this] {
            for (const auto& job : jobs) if (!job.second) return false;
            return true;
        });
    }
    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        stopped = true;
        for (const auto& job : jobs) job.first->store(true);
        changed.notify_all();
    }
    // Call from CPU loops, SQLite's progress handler and curl's progress
    // callback. Background budget: 2 ms work / 38 ms rest, 256 KiB/s downloads.
    // These are cooperative limits, not a hard real-time I/O guarantee.
    void checkpoint(const Cancel& cancel, size_t bytes = 0) {
        std::unique_lock<std::mutex> lock(mutex);
        auto job = jobs.find(cancel);
        if (job == jobs.end()) return;
        for (;;) {
            const auto mode = modeLocked();
            if (mode == Mode::Stopped || cancel->load()) return;
            if (mode == Mode::Foreground) {
                burst = {}; cpuUntil = {}; networkUntil = {};
                job->second = false;
                return;
            }
            job->second = true;
            changed.notify_all();
            if (mode == Mode::Parked) {
                // Periodically reevaluate the stable-playback grace period.
                changed.wait_for(lock, std::chrono::milliseconds(100));
                burst = {}; cpuUntil = {}; networkUntil = {};
                continue;
            }
            const auto now = Clock::now();
            if (bytes) {
                networkUntil = std::max(now, networkUntil) +
                    std::chrono::microseconds(uint64_t(bytes) * 1000000 / (256 * 1024));
                bytes = 0;
            }
            if (burst != Clock::time_point{} && now - burst >= std::chrono::milliseconds(2)) {
                // Include overruns (e.g. a slow filesystem call), then persist
                // the rest deadline across wakeups. A past window must never
                // leave the writer running without further throttling.
                cpuUntil = now + (now - burst) * 19;
                burst = {};
            }
            const auto until = std::max(cpuUntil, networkUntil);
            if (until > now) {
                burst = {}; // waiting is not part of the next work burst
                changed.wait_until(lock, until);
                continue;
            }
            if (burst == Clock::time_point{}) burst = now;
            job->second = false;
            return;
        }
    }
private:
    Mode modeLocked() const {
        if (stopped) return Mode::Stopped;
        if (players.empty()) return Mode::Foreground;
        for (const auto& player : players)
            if (player.second == Clock::time_point{} || Clock::now() - player.second < grace) return Mode::Parked;
        return Mode::Background;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::map<Cancel, bool> jobs;
    std::map<uint64_t, Clock::time_point> players;
    uint64_t nextPlayer = 0;
    bool stopped = false;
    std::chrono::milliseconds grace;
    Clock::time_point burst{}, cpuUntil{}, networkUntil{};
};

} // namespace stremio::archive
