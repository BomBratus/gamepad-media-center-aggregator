#pragma once

#include "api/stremio/playback_history.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace stremio {

/// Queue playback snapshots and ordered mutations for one background writer.
/// Synchronous reads and clears enqueue barriers before using the store.
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
        const std::string cacheScope = snapshot.scope;
        const std::string cacheId = snapshot.item.ratingKey;
        const nlohmann::json record = cachedRecord(snapshot);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            const uint64_t sequence = ++queueSequence_;
            if (!queue_.empty() && queue_.back().kind == Job::Kind::Save &&
                queue_.back().snapshot.scope == snapshot.scope &&
                queue_.back().snapshot.item.ratingKey == snapshot.item.ratingKey) {
                // Only replace the queued tail. A popped job is already being
                // persisted, and any intervening job preserves its FIFO meaning.
                queue_.back().snapshot = std::move(snapshot);
            } else {
                queue_.push_back(Job{Job::Kind::Save, std::move(snapshot), {}, {}});
            }
            storeCachedRecord(cacheScope, cacheId, record, sequence);
        }
        queueChanged_.notify_one();
    }

    /// Run a mutation on the writer thread in FIFO order with playback saves.
    /// This also fences save coalescing on either side of the callback. An
    /// operation must own its captured data and must not call this instance's
    /// synchronous read/clear/flush APIs. Exceptions use the failure callback.
    void enqueue(std::function<void()> operation) {
        if (!operation) return;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            ++queueSequence_;
            queue_.push_back(Job{Job::Kind::Callback, {}, {}, std::move(operation)});
        }
        queueChanged_.notify_one();
    }

    /// Clear an entry on the writer thread. Cached metadata remains available
    /// immediately, with its resume position set to zero.
    void clearAsync(std::string scope, std::string id) {
        if (scope.empty() || id.empty()) return;

        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            const uint64_t sequence = ++queueSequence_;
            queue_.push_back(Job{Job::Kind::Callback, {}, {},
                [this, scope, id, sequence] {
                    const auto beforeClear = history_.load(scope, id);
                    if (beforeClear.empty()) return;
                    if (!history_.clear(scope, id)) {
                        reportFailure();
                        return;
                    }
                    auto cleared = history_.load(scope, id);
                    if (cleared.empty()) {
                        cleared = beforeClear;
                        cleared["position"] = 0;
                        cleared["updated"] = playback_history_detail::epochMilliseconds();
                    }
                    storeCachedRecord(scope, id, std::move(cleared), sequence);
                }});
            markCachedClear(scope, id, sequence);
        }
        queueChanged_.notify_one();
    }

    /// Finish earlier saves before a restart that bypasses static destruction.
    void flush() {
        auto ticket = enqueueBarrier();
        ticket.barrier->waitUntilReached();
        BarrierRelease release{ticket.barrier};
    }

    nlohmann::json load(const std::string& scope, const std::string& id) {
        auto ticket = enqueueBarrier();
        ticket.barrier->waitUntilReached();
        BarrierRelease release{ticket.barrier};
        auto record = history_.load(scope, id);
        if (record.empty())
            eraseCachedRecord(scope, id, ticket.sequence);
        else
            storeCachedRecord(scope, id, record, ticket.sequence);
        return record;
    }

    nlohmann::json records(const std::string& scope) {
        auto ticket = enqueueBarrier();
        ticket.barrier->waitUntilReached();
        BarrierRelease release{ticket.barrier};
        auto loaded = history_.records(scope);
        storeCachedRecords(scope, loaded, ticket.sequence);
        return loaded;
    }

    bool clear(const std::string& scope, const std::string& id) {
        auto ticket = enqueueBarrier();
        ticket.barrier->waitUntilReached();
        BarrierRelease release{ticket.barrier};
        if (!history_.clear(scope, id)) return false;
        auto record = history_.load(scope, id);
        storeCachedRecord(scope, id, std::move(record), ticket.sequence);
        return true;
    }

    /// Return a cached record without waiting for the writer or reading disk.
    /// A key not loaded or saved in this process returns an empty object.
    nlohmann::json cachedLoad(const std::string& scope, const std::string& id) const {
        if (scope.empty() || id.empty()) return nlohmann::json::object();
        std::lock_guard<std::mutex> lock(cacheMutex_);
        const auto scopeIt = cache_.find(scope);
        if (scopeIt == cache_.end()) return nlohmann::json::object();
        const auto recordIt = scopeIt->second.find(id);
        return recordIt == scopeIt->second.end() ? nlohmann::json::object() : recordIt->second.value;
    }

private:
    struct Snapshot {
        std::string scope;
        media::Item item;
        media::Media source;
        int64_t position;
        int64_t duration;
    };

    struct CacheEntry {
        nlohmann::json value;
        uint64_t sequence = 0;
    };

    using ScopeCache = std::map<std::string, CacheEntry>;

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
        enum class Kind { Save, Barrier, Callback };
        Kind kind = Kind::Save;
        Snapshot snapshot{};
        std::shared_ptr<Barrier> barrier;
        std::function<void()> operation;
    };

    struct BarrierRelease {
        std::shared_ptr<Barrier> barrier;
        ~BarrierRelease() { barrier->release(); }
    };

    struct BarrierTicket {
        std::shared_ptr<Barrier> barrier;
        uint64_t sequence;
    };

    static nlohmann::json cachedRecord(const Snapshot& snapshot) {
        nlohmann::json metadata = {
            {"ratingKey", snapshot.item.ratingKey},
            {"type", snapshot.item.type},
            {"title", snapshot.item.title},
            {"thumb", snapshot.item.thumb},
            {"guid", snapshot.item.guid},
        };
        if (!snapshot.item.grandparentRatingKey.empty())
            metadata["grandparentRatingKey"] = snapshot.item.grandparentRatingKey;
        if (!snapshot.item.grandparentTitle.empty()) metadata["grandparentTitle"] = snapshot.item.grandparentTitle;
        if (!snapshot.item.grandparentThumb.empty()) metadata["grandparentThumb"] = snapshot.item.grandparentThumb;
        if (!snapshot.item.grandparentArt.empty()) metadata["grandparentArt"] = snapshot.item.grandparentArt;
        return {
            {"position", snapshot.position},
            {"duration", snapshot.duration},
            {"item", std::move(metadata)},
            {"source", {
                {"url", playback_history_detail::sourceUrl(snapshot.source)},
                {"identity", snapshot.source.sourceIdentity},
            }},
            {"updated", playback_history_detail::epochMilliseconds()},
        };
    }

    void storeCachedRecord(const std::string& scope, const std::string& id,
        nlohmann::json record, uint64_t sequence) {
        if (scope.empty() || id.empty()) return;
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto& entry = cache_[scope][id];
        if (entry.sequence <= sequence) {
            entry.value = std::move(record);
            entry.sequence = sequence;
        }
    }

    void eraseCachedRecord(const std::string& scope, const std::string& id, uint64_t sequence) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        const auto scopeIt = cache_.find(scope);
        if (scopeIt == cache_.end()) return;
        const auto recordIt = scopeIt->second.find(id);
        if (recordIt != scopeIt->second.end() && recordIt->second.sequence <= sequence)
            scopeIt->second.erase(recordIt);
        if (scopeIt->second.empty()) cache_.erase(scopeIt);
    }

    void storeCachedRecords(const std::string& scope, const nlohmann::json& records, uint64_t sequence) {
        if (scope.empty()) return;
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto& scopeCache = cache_[scope];
        for (auto it = scopeCache.begin(); it != scopeCache.end();) {
            if (it->second.sequence <= sequence && (!records.is_object() || !records.contains(it->first)))
                it = scopeCache.erase(it);
            else
                ++it;
        }
        if (!records.is_object()) return;
        for (auto it = records.begin(); it != records.end(); ++it) {
            auto& entry = scopeCache[it.key()];
            if (entry.sequence <= sequence) {
                entry.value = it.value();
                entry.sequence = sequence;
            }
        }
    }

    void markCachedClear(const std::string& scope, const std::string& id, uint64_t sequence) {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        const auto scopeIt = cache_.find(scope);
        if (scopeIt == cache_.end()) return;
        const auto recordIt = scopeIt->second.find(id);
        if (recordIt == scopeIt->second.end() || recordIt->second.sequence > sequence ||
            !recordIt->second.value.is_object())
            return;
        recordIt->second.value["position"] = 0;
        recordIt->second.value["updated"] = playback_history_detail::epochMilliseconds();
        recordIt->second.sequence = sequence;
    }

    BarrierTicket enqueueBarrier() {
        auto barrier = std::make_shared<Barrier>();
        uint64_t sequence;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            sequence = ++queueSequence_;
            queue_.push_back(Job{Job::Kind::Barrier, {}, barrier});
        }
        queueChanged_.notify_one();
        return BarrierTicket{std::move(barrier), sequence};
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
                if (job.kind == Job::Kind::Save) {
                    if (!history_.save(job.snapshot.scope, std::move(job.snapshot.item),
                            std::move(job.snapshot.source), job.snapshot.position, job.snapshot.duration))
                        reportFailure();
                } else {
                    job.operation();
                }
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
    uint64_t queueSequence_ = 0;
    mutable std::mutex cacheMutex_;
    std::map<std::string, ScopeCache> cache_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace stremio
