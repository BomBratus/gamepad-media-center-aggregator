/*
    GMCA — bounded request coalescing for Stremio addon fan-out.

    Stremio frequently asks several independent addons for the same logical
    operation (manifests, metadata, streams, subtitles, catalog rows, search).
    The backend consumes those responses in addon order, but issuing each HTTP
    request only after the previous one completed made total latency the SUM of
    every addon latency/timeout.

    This helper keeps the observable ordering/error behaviour unchanged while
    starting small batches concurrently on GMCA's existing cross-platform
    ThreadPool. Batches are registered lazily: no network work starts until the
    first matching get() is actually consumed. That is important for synchronous
    routing helpers such as sectionTabs(), which may enumerate catalogs without
    fetching them.
*/

#pragma once

#include "api/http.hpp"
#include "utils/thread.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace stremio {
namespace requests {

namespace detail {

struct Result {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::string body;
    std::exception_ptr error;
};

struct BatchRequest {
    std::string url;
    std::string key;
    std::shared_ptr<Result> result;
};

struct Batch {
    std::mutex startMutex;
    bool started = false;
    long timeout = HTTP::TIMEOUT;
    std::vector<BatchRequest> requests;
};

struct ExactRegistration {
    std::shared_ptr<Batch> batch;
    std::chrono::steady_clock::time_point expires;
};

struct SearchFamily {
    std::vector<std::string> roots;
    long timeout = HTTP::TIMEOUT;
    std::chrono::steady_clock::time_point expires;
};

struct CachedResponse {
    std::string body;
    std::chrono::steady_clock::time_point expires;
};

struct Registry {
    std::mutex mutex;
    std::unordered_map<std::string, ExactRegistration> exact;
    std::unordered_map<std::string, std::shared_ptr<Result>> inflight;
    std::unordered_map<std::string, CachedResponse> cache;
    std::vector<SearchFamily> searchFamilies;
};

inline Registry& registry() {
    static Registry value;
    return value;
}

inline std::string requestKey(const std::string& url, long timeout) {
    // URLs cannot contain a literal newline in an HTTP request, so this is an
    // unambiguous and allocation-light compound key.
    return url + "\n" + std::to_string(timeout);
}

inline size_t batchWidth() {
    // Reuse the platform-tuned worker count (Vita=2, most console targets=4),
    // but never flood a desktop with hardware_concurrency() requests at once.
    size_t width = ThreadPool::max_thread_num;
    if (width == 0) width = 1;
    return std::min<size_t>(width, 4);
}

inline void cleanupLocked(Registry& r, std::chrono::steady_clock::time_point now) {
    for (auto it = r.exact.begin(); it != r.exact.end();) {
        if (it->second.expires <= now)
            it = r.exact.erase(it);
        else
            ++it;
    }
    r.searchFamilies.erase(
        std::remove_if(r.searchFamilies.begin(), r.searchFamilies.end(),
            [now](const SearchFamily& family) { return family.expires <= now; }),
        r.searchFamilies.end());
    for (auto it = r.cache.begin(); it != r.cache.end();) {
        if (it->second.expires <= now)
            it = r.cache.erase(it);
        else
            ++it;
    }
}

inline std::pair<std::shared_ptr<Batch>, std::shared_ptr<Result>> findExact(
    const std::string& key, std::chrono::steady_clock::time_point now) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    cleanupLocked(r, now);
    auto it = r.exact.find(key);
    if (it == r.exact.end()) return {};

    std::shared_ptr<Batch> batch = it->second.batch;
    for (const auto& request : batch->requests)
        if (request.key == key) return {batch, request.result};
    return {};
}

inline void startBatch(const std::shared_ptr<Batch>& batch) {
    {
        std::lock_guard<std::mutex> lock(batch->startMutex);
        if (batch->started) return;
        batch->started = true;
    }

    for (const auto& request : batch->requests) {
        std::string url = request.url;
        std::shared_ptr<Result> result = request.result;
        long timeout = batch->timeout;
        ThreadPool::instance().submit([url, result, timeout](HTTP&) {
            std::string body;
            std::exception_ptr error;
            try {
                body = HTTP::get(url, HTTP::Timeout{timeout});
            } catch (...) {
                error = std::current_exception();
            }

            {
                std::lock_guard<std::mutex> lock(result->mutex);
                result->body = std::move(body);
                result->error = error;
                result->done = true;
            }
            result->cv.notify_all();
        });
    }
}

}  // namespace detail

/// Register exact URLs that will shortly be consumed one-by-one. Registration
/// itself performs no I/O. Requests are split into small groups so a large addon
/// collection cannot monopolize the global worker pool or memory on consoles.
inline void registerBatch(const std::vector<std::string>& urls, long timeout = HTTP::TIMEOUT) {
    size_t width = detail::batchWidth();
    if (width < 2 || urls.size() < 2) return;

    std::vector<std::string> unique;
    unique.reserve(urls.size());
    for (const auto& url : urls) {
        if (url.empty()) continue;
        if (std::find(unique.begin(), unique.end(), url) == unique.end()) unique.push_back(url);
    }
    if (unique.size() < 2) return;

    auto now = std::chrono::steady_clock::now();
    auto expires = now + std::chrono::seconds(30);
    detail::Registry& r = detail::registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    detail::cleanupLocked(r, now);

    for (size_t first = 0; first < unique.size(); first += width) {
        size_t last = std::min(unique.size(), first + width);
        std::vector<std::pair<std::string, std::string>> candidates;
        candidates.reserve(last - first);
        for (size_t i = first; i < last; ++i) {
            std::string key = detail::requestKey(unique[i], timeout);
            // Keep an already-live registration: this also coalesces overlapping
            // calls from two backend operations instead of duplicating them.
            if (r.exact.find(key) == r.exact.end()) candidates.emplace_back(unique[i], std::move(key));
        }
        if (candidates.size() < 2) continue;

        auto batch = std::make_shared<detail::Batch>();
        batch->timeout = timeout;
        batch->requests.reserve(candidates.size());
        for (auto& candidate : candidates) {
            detail::BatchRequest request;
            request.url = std::move(candidate.first);
            request.key = std::move(candidate.second);
            request.result = std::make_shared<detail::Result>();
            batch->requests.push_back(std::move(request));
        }
        for (const auto& request : batch->requests) r.exact[request.key] = {batch, expires};
    }
}

/// Register catalog URL roots that support Stremio's `search` extra. The query
/// text is not known in AddonEngine::allCatalogs(), so the concrete URLs are
/// materialized only when the first `/search=...` request is seen. Families are
/// grouped by type by the caller, avoiding movie requests during a series-only
/// search (and vice versa).
inline void registerSearchBatch(const std::vector<std::string>& roots, long timeout = HTTP::TIMEOUT) {
    if (detail::batchWidth() < 2 || roots.size() < 2) return;

    std::vector<std::string> unique;
    unique.reserve(roots.size());
    for (const auto& root : roots) {
        if (root.empty()) continue;
        if (std::find(unique.begin(), unique.end(), root) == unique.end()) unique.push_back(root);
    }
    if (unique.size() < 2) return;

    auto now = std::chrono::steady_clock::now();
    auto expires = now + std::chrono::seconds(30);
    detail::Registry& r = detail::registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    detail::cleanupLocked(r, now);
    for (auto& family : r.searchFamilies) {
        if (family.timeout == timeout && family.roots == unique) {
            family.expires = expires;
            return;
        }
    }
    r.searchFamilies.push_back({std::move(unique), timeout, expires});
}

/// Fetch one URL. If AddonEngine registered it as part of a fan-out, start the
/// bounded batch and wait only for this response. Backend code still consumes
/// responses and errors in the original addon order, so this changes latency,
/// not result precedence or fallback semantics.
inline std::string get(const std::string& url, long timeout = HTTP::TIMEOUT) {
    std::string key = detail::requestKey(url, timeout);
    auto now = std::chrono::steady_clock::now();
    auto exact = detail::findExact(key, now);

    if (!exact.first) {
        // Search requests cannot be registered as exact URLs until the query is
        // known. Match a short-lived family and materialize the same suffix for
        // every searchable catalog of that type.
        std::vector<std::string> roots;
        {
            detail::Registry& r = detail::registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            detail::cleanupLocked(r, now);
            for (const auto& family : r.searchFamilies) {
                if (family.timeout != timeout) continue;
                for (const auto& root : family.roots) {
                    if (url.size() <= root.size() || url.compare(0, root.size(), root) != 0) continue;
                    std::string suffix = url.substr(root.size());
                    if (suffix.rfind("/search=", 0) == 0) {
                        roots = family.roots;
                        break;
                    }
                }
                if (!roots.empty()) break;
            }
        }

        if (!roots.empty()) {
            // Recover the suffix from the matching root, then fan the same query
            // out across the family. registerBatch() applies the platform bound.
            std::string suffix;
            for (const auto& root : roots)
                if (url.size() > root.size() && url.compare(0, root.size(), root) == 0) {
                    suffix = url.substr(root.size());
                    break;
                }
            if (!suffix.empty()) {
                std::vector<std::string> urls;
                urls.reserve(roots.size());
                for (const auto& root : roots) urls.push_back(root + suffix);
                registerBatch(urls, timeout);
                exact = detail::findExact(key, std::chrono::steady_clock::now());
            }
        }
    }

    if (!exact.first || !exact.second) {
        // Even a one-addon resource can be requested concurrently by several
        // page sections (series detail, seasons, next-up). Coalesce identical
        // in-flight URLs so those callers share one HTTP request instead of
        // occupying multiple workers with the same round-trip.
        std::shared_ptr<detail::Result> single;
        bool owner = false;
        {
            detail::Registry& r = detail::registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            detail::cleanupLocked(r, now);
            auto it = r.inflight.find(key);
            if (it != r.inflight.end()) {
                single = it->second;
            } else {
                single = std::make_shared<detail::Result>();
                r.inflight[key] = single;
                owner = true;
            }
        }

        if (owner) {
            std::string body;
            std::exception_ptr requestError;
            try {
                body = HTTP::get(url, HTTP::Timeout{timeout});
            } catch (...) {
                requestError = std::current_exception();
            }
            {
                std::lock_guard<std::mutex> lock(single->mutex);
                single->body = std::move(body);
                single->error = requestError;
                single->done = true;
            }
            single->cv.notify_all();

            detail::Registry& r = detail::registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            auto it = r.inflight.find(key);
            if (it != r.inflight.end() && it->second == single) r.inflight.erase(it);
        }

        std::string body;
        std::exception_ptr requestError;
        {
            std::unique_lock<std::mutex> lock(single->mutex);
            single->cv.wait(lock, [&single]() { return single->done; });
            body = single->body;
            requestError = single->error;
        }
        if (requestError) std::rethrow_exception(requestError);
        return body;
    }

    detail::startBatch(exact.first);
    std::string body;
    std::exception_ptr error;
    {
        std::unique_lock<std::mutex> lock(exact.second->mutex);
        exact.second->cv.wait(lock, [&exact]() { return exact.second->done; });
        body = exact.second->body;
        error = exact.second->error;
    }

    // This is request coalescing, not a persistent response cache. Drop the
    // registration after consumption; a simultaneous waiter already owns the
    // shared Batch/Result and remains safe.
    {
        detail::Registry& r = detail::registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.exact.find(key);
        if (it != r.exact.end() && it->second.batch == exact.first) r.exact.erase(it);
    }

    if (error) std::rethrow_exception(error);
    return body;
}

/// Fetch with a short successful-response cache. Metadata is effectively static
/// during a browsing session, and season navigation commonly asks for the same
/// series meta again when an episode is selected. Keep the cache tiny and
/// time-bounded so addon reconfiguration is observed quickly.
inline std::string getCached(
    const std::string& url, long timeout = HTTP::TIMEOUT, long ttlMs = 60000) {
    std::string key = detail::requestKey(url, timeout);
    auto now = std::chrono::steady_clock::now();
    {
        detail::Registry& r = detail::registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        detail::cleanupLocked(r, now);
        auto it = r.cache.find(key);
        if (it != r.cache.end()) return it->second.body;
    }

    std::string body = get(url, timeout);
    if (body.empty() || ttlMs <= 0) return body;

    detail::Registry& r = detail::registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    detail::cleanupLocked(r, std::chrono::steady_clock::now());
    // Console-friendly bound: enough for a few recently browsed shows/movies,
    // without turning this into a long-lived catalog cache.
    if (r.cache.size() >= 32) r.cache.erase(r.cache.begin());
    r.cache[key] = {body, std::chrono::steady_clock::now() + std::chrono::milliseconds(ttlMs)};
    return body;
}

/// Forget unconsumed registrations after the configured addon set changes.
/// In-flight callers keep shared ownership of their Batch/Result and are not
/// cancelled or invalidated.
inline void clear() {
    detail::Registry& r = detail::registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.exact.clear();
    r.inflight.clear();
    r.cache.clear();
    r.searchFamilies.clear();
}

}  // namespace requests
}  // namespace stremio
