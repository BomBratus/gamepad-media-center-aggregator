#pragma once

#include "api/stremio/playback_history.hpp"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace stremio {

/// Queue playback snapshots for one background writer. Reads and clears enqueue
/// barriers and then call the existing synchronous store after earlier writes.
class AsyncPlaybackHistory {
public:
    using FailureCallback = std::function<void()>;

    explicit AsyncPlaybackHistory(std::string path, FailureCallback onFailure = {})
        : history_(std::move(path)), onFailure_(std::move(onFailure)), worker_([this] { run(); }) {}

    ~AsyncPlaybackHistory() {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopping_ = true;
        }
        queueChanged_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    AsyncPlaybackHistory(const AsyncPlaybackHistory&) = delete;
    AsyncPlaybackHistory& operator=(const AsyncPlaybackHistory&) = delete;

    /// Capture a snapshot and return after enqueueing it.
    void save(std::string scope, media::Item item, media::Media source, int64_t position, int64_t duration) {
        Snapshot snapshot{std::move(scope), std::move(item), std::move(source), position, duration};
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(Job{Job::Kind::Save, std::move(snapshot), {}});
        }
        queueChanged_.notify_one();
    }

    /// Finish earlier saves before a restart that bypasses static destruction.
    void flush() {
        auto barrier = enqueueBarrier();
        barrier->waitUntilReached();
        BarrierRelease release{barrier};
    }

    nlohmann::json load(const std::string& scope, const std::string& id) {
        auto barrier = enqueueBarrier();
        barrier->waitUntilReached();
        BarrierRelease release{barrier};
        return history_.load(scope, id);
    }

    nlohmann::json records(const std::string& scope) {
        auto barrier = enqueueBarrier();
        barrier->waitUntilReached();
        BarrierRelease release{barrier};
        return history_.records(scope);
    }

    bool clear(const std::string& scope, const std::string& id) {
        auto barrier = enqueueBarrier();
        barrier->waitUntilReached();
        BarrierRelease release{barrier};
        return history_.clear(scope, id);
    }

private:
    struct Snapshot {
        std::string scope;
        media::Item item;
        media::Media source;
        int64_t position;
        int64_t duration;
    };

    struct Barrier {
        void waitUntilReached() {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this] { return reached; });
        }

        void markReached() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                reached = true;
            }
            changed.notify_all();
        }

        void waitUntilReleased() {
            std::unique_lock<std::mutex> lock(mutex);
            changed.wait(lock, [this] { return released; });
        }

        void release() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                released = true;
            }
            changed.notify_all();
        }

        std::mutex mutex;
        std::condition_variable changed;
        bool reached = false;
        bool released = false;
    };

    struct Job {
        enum class Kind { Save, Barrier };
        Kind kind = Kind::Save;
        Snapshot snapshot{};
        std::shared_ptr<Barrier> barrier;
    };

    struct BarrierRelease {
        std::shared_ptr<Barrier> barrier;
        ~BarrierRelease() { barrier->release(); }
    };

    std::shared_ptr<Barrier> enqueueBarrier() {
        auto barrier = std::make_shared<Barrier>();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queue_.push_back(Job{Job::Kind::Barrier, {}, barrier});
        }
        queueChanged_.notify_one();
        return barrier;
    }

    void reportFailure() noexcept {
        if (!onFailure_) return;
        try {
            onFailure_();
        } catch (...) {
            // Reporting must not let an I/O or logger failure escape the worker.
        }
    }

    void run() noexcept {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueChanged_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                job = std::move(queue_.front());
                queue_.pop_front();
            }

            if (job.kind == Job::Kind::Barrier) {
                job.barrier->markReached();
                job.barrier->waitUntilReleased();
                continue;
            }

            try {
                if (!history_.save(job.snapshot.scope, std::move(job.snapshot.item),
                        std::move(job.snapshot.source), job.snapshot.position, job.snapshot.duration))
                    reportFailure();
            } catch (...) {
                reportFailure();
            }
        }
    }

    PlaybackHistory history_;
    FailureCallback onFailure_;
    std::mutex queueMutex_;
    std::condition_variable queueChanged_;
    std::deque<Job> queue_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace stremio
