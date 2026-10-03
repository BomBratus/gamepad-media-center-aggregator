#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace stremio::archive {

// Playback cancels background passes, then waits off the UI thread until their
// checkpoints and temporary catalog copies have been released.
class PlaybackGate {
public:
    using Cancel = std::shared_ptr<std::atomic_bool>;

    Cancel start() {
        std::lock_guard<std::mutex> lock(mutex);
        if (players) { pending = true; return {}; }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        jobs.push_back(cancel);
        return cancel;
    }

    void finish(const Cancel& cancel) {
        std::lock_guard<std::mutex> lock(mutex);
        jobs.erase(std::remove(jobs.begin(), jobs.end(), cancel), jobs.end());
        idle.notify_all();
    }

    void pause() {
        std::lock_guard<std::mutex> lock(mutex);
        ++players;
        if (!jobs.empty()) pending = true;
        for (const auto& cancel : jobs) cancel->store(true);
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        idle.wait(lock, [this] { return jobs.empty(); });
    }

    bool resume() {
        std::lock_guard<std::mutex> lock(mutex);
        if (!players || --players) return false;
        const bool restart = pending;
        pending = false;
        return restart;
    }

private:
    std::mutex mutex;
    std::condition_variable idle;
    std::vector<Cancel> jobs;
    size_t players = 0;
    bool pending = false;
};

} // namespace stremio::archive
