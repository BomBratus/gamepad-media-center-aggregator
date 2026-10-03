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
    if (limit || random) ps4diag::write("archive query queued");
    queries.submit([job, filter, offset, limit, random, callback, snapshot] {
        Result result;
        if (limit || random) ps4diag::write("archive query begin");
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
                auto found = data->index->query(filter, offset, limit, random);
                result.indexed = found.indexed; result.total = found.total;
                result.options.genres = std::move(found.genres);
                result.options.hasVotes = true;
                for (const auto& record : found.records) result.items.push_back(parseMetaPreview(record.meta));
            } else result.indexed = job->building;
        } catch (const std::exception& detail) {
            ps4diag::write("archive query failed detail=" + std::string(detail.what()));
            result.error = "main/archive/cache_error";
        } catch (...) { result.error = "main/archive/cache_error"; }
        if (limit || random) ps4diag::write("archive query complete items=" + std::to_string(result.items.size()));
        brls::sync([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
    });
}
} // namespace stremio::archive
