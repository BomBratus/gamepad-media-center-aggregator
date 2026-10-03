#include "api/stremio/archive.hpp"
#include "api/stremio/archive_storage.hpp"
#include "api/stremio/imdb_index.hpp"
#include "api/stremio/types.hpp"
#include "utils/config.hpp"
#include "utils/thread.hpp"
#include "utils/ps4_diagnostics.hpp"
#include <borealis/core/thread.hpp>
#include <borealis/core/application.hpp>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace stremio::archive {
namespace {
int64_t now() { return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); }
constexpr int64_t refreshAge = 7 * 86400;
void downloadDataset(const std::string& name, const std::string& path, const IndexCancel& cancel) {
    const auto temporary = path + ".part";
    try {
        ps4diag::write("archive dataset-download begin name=" + name);
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot save IMDb dataset");
        HTTP request;
        HTTP::set_option(request, HTTP::Timeout{1800000, 10000}, cancel,
            HTTP::Header{"Accept-Encoding: identity"});
        request._get("https://datasets.imdbws.com/" + name, &file);
        if (!file.good()) throw std::runtime_error("Cannot write IMDb dataset");
        file.close();
        if (file.fail() || cancel->load()) throw std::runtime_error("IMDb download interrupted");
        if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("Cannot save IMDb dataset");
        ps4diag::write("archive dataset-download complete name=" + name);
    } catch (...) { std::remove(temporary.c_str()); throw; }
}
} // namespace

struct Cache::State {
    std::string path;
    std::once_flag loaded;
    std::mutex mutex;
    std::shared_ptr<const Snapshot> snapshot = std::make_shared<Snapshot>();
    std::atomic_bool refreshing{false};
    std::atomic<int64_t> nextCheck{0};
    std::atomic<size_t> building{0};
    std::string error;
    void load() {
        std::call_once(loaded, [this] {
            if (!std::ifstream(path).good()) return;
            try {
                ps4diag::write("archive index-load begin");
                auto value = std::make_shared<Snapshot>();
                value->index = std::make_shared<ImdbIndex>(path);
                value->refreshed = value->index->query({}, 0, 0, false).refreshed;
                std::lock_guard<std::mutex> guard(mutex);
                snapshot = std::move(value);
                ps4diag::write("archive index-load complete");
            } catch (const std::exception& detail) {
                ps4diag::write("archive index-load failed detail=" + std::string(detail.what()));
                std::lock_guard<std::mutex> guard(mutex);
                error = "main/archive/cache_error";
            }
        });
    }
};

Cache& Cache::instance() { static Cache cache; return cache; }
Cache::Cache() {
    brls::Application::getExitEvent()->subscribe([this] {
        exiting = true;
        playbackGate.pause();
        queries.stop();
    });
}
std::shared_ptr<Cache::State> Cache::current() {
    if (!state) {
        state = std::make_shared<State>();
        // Public IMDb metadata is shared across profiles; old scoped addon
        // archive files remain untouched and are no longer loaded into memory.
        state->path = AppConfig::instance().configDir() + "/imdb-index-v1.sqlite";
    }
    return state;
}
void Cache::refresh(bool force) {
    if (exiting) return;
    auto job = current();
    if (!force && now() < job->nextCheck) return;
    if (job->refreshing.exchange(true)) return;
    auto cancel = playbackGate.start();
    if (!cancel) { job->refreshing = false; return; }
    ThreadPool::instance().submit([this, job, force, cancel](HTTP&) {
        struct Release {
            std::shared_ptr<State> job;
            PlaybackGate& gate;
            IndexCancel cancel;
            ~Release() { job->refreshing = false; gate.finish(cancel); }
        } release{job, playbackGate, cancel};
        if (cancel->load()) return;
        job->load();
        {
            std::lock_guard<std::mutex> guard(job->mutex);
            if (!force && job->snapshot->refreshed && now() - job->snapshot->refreshed < refreshAge) {
                job->nextCheck = job->snapshot->refreshed + refreshAge;
                return;
            }
            job->nextCheck = now() + 600;
            job->error.clear();
        }
        try {
            ps4diag::write("archive build begin");
            if (!buildImdbIndex(job->path, cancel, downloadDataset,
                    [job](size_t count) { job->building = count; })) return;
            auto next = std::make_shared<Snapshot>();
            next->index = std::make_shared<ImdbIndex>(job->path);
            next->refreshed = next->index->query({}, 0, 0, false).refreshed;
            std::lock_guard<std::mutex> guard(job->mutex);
            job->snapshot = std::move(next);
            job->nextCheck = now() + refreshAge;
            job->building = 0;
            ps4diag::write("archive build complete");
        } catch (const std::exception& error) {
            if (!cancel->load()) {
                // Our generated importer errors and curl status messages contain
                // no account credentials or media URLs. Preserve the real cause.
                ps4diag::write("archive build failed detail=" + std::string(error.what()));
                std::lock_guard<std::mutex> guard(job->mutex);
                job->error = "main/archive/refresh_error";
            }
        } catch (...) {
            if (!cancel->load()) {
                ps4diag::write("archive build failed detail=unknown");
                std::lock_guard<std::mutex> guard(job->mutex);
                job->error = "main/archive/refresh_error";
            }
        }
    });
}
void Cache::pauseForPlayback() { playbackGate.pause(); }
void Cache::waitForPlayback() { playbackGate.wait(); }
void Cache::resumeAfterPlayback() {
    if (playbackGate.resume() && !exiting) refresh(true);
}
void Cache::query(const Filter& filter, size_t offset, size_t limit, bool random,
        std::function<void(Result)> callback, std::shared_ptr<const Snapshot> snapshot) {
    auto job = current();
    const bool interactive = limit || random;
    if (interactive) ps4diag::write("archive query queued");
    const auto queued = std::chrono::steady_clock::now();

    auto execute = [job, filter, offset, limit, random, callback, snapshot, queued, interactive](
                       const IndexCancel& cancel) {
        Result result;
        const auto started = std::chrono::steady_clock::now();
        const auto queueMs = std::chrono::duration_cast<std::chrono::milliseconds>(started - queued).count();
        int64_t sqlMs = 0;
        bool measuringSql = false;
        std::chrono::steady_clock::time_point sqlStarted;
        if (interactive)
            ps4diag::write("archive query begin queue-ms=" + std::to_string(queueMs));

        if (cancel && cancel->load()) {
            if (interactive) {
                const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - queued).count();
                ps4diag::write("archive query cancelled items=0 queue-ms=" + std::to_string(queueMs) +
                    " sql-ms=0 total-ms=" + std::to_string(totalMs));
            }
            brls::sync([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
            return;
        }

        try {
            job->load();
            std::shared_ptr<const Snapshot> data;
            {
                std::lock_guard<std::mutex> guard(job->mutex);
                data = snapshot ? snapshot : job->snapshot;
                result.error = job->error;
            }
            result.refreshing = job->refreshing;
            result.refreshed = data->refreshed;
            result.snapshot = data;
            if (data->index) {
                measuringSql = true;
                sqlStarted = std::chrono::steady_clock::now();
                auto found = data->index->query(filter, offset, limit, random, cancel);
                sqlMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - sqlStarted).count();
                measuringSql = false;
                result.indexed = found.indexed; result.total = found.total;
                result.options.genres = std::move(found.genres);
                result.options.hasVotes = true;
                for (const auto& record : found.records) result.items.push_back(parseMetaPreview(record.meta));
            } else result.indexed = job->building;
        } catch (const std::exception& detail) {
            if (measuringSql)
                sqlMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - sqlStarted).count();
            if (!(cancel && cancel->load())) {
                ps4diag::write("archive query failed detail=" + std::string(detail.what()));
                result.error = "main/archive/cache_error";
            }
        } catch (...) {
            if (measuringSql)
                sqlMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - sqlStarted).count();
            if (!(cancel && cancel->load())) result.error = "main/archive/cache_error";
        }

        if (interactive) {
            const auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - queued).count();
            const bool cancelled = cancel && cancel->load();
            ps4diag::write(std::string("archive query ") + (cancelled ? "cancelled" : "complete") +
                " items=" + std::to_string(result.items.size()) +
                " queue-ms=" + std::to_string(queueMs) +
                " sql-ms=" + std::to_string(sqlMs) +
                " total-ms=" + std::to_string(totalMs));
        }
        brls::sync([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
    };

    if (interactive) {
        queries.submitLatest(std::move(execute));
    } else {
        queries.submit([execute = std::move(execute)]() mutable { execute({}); });
    }
}
} // namespace stremio::archive
