#include "api/stremio/archive.hpp"
#include "api/stremio/archive_storage.hpp"
#include "api/stremio/imdb_index.hpp"
#include "api/stremio/types.hpp"
#include "utils/config.hpp"
#include "utils/background_governor.hpp"
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
void downloadDataset(const std::string& name, const std::string& path, const IndexCancel& cancel, PlaybackGate& gate) {
    const auto temporary = path + ".part";
    try {
        ps4diag::write("archive dataset-download begin name=" + name);
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot save IMDb dataset");
        HTTP request;
        HTTP::set_option(request, HTTP::Timeout{0, 10000}, cancel,
            HTTP::Header{"Accept-Encoding: identity"},
            HTTP::Progress::Callback{[&gate, cancel, previous = curl_off_t(0)](curl_off_t, curl_off_t current) mutable {
                const auto bytes = current > previous ? size_t(current - previous) : 0;
                previous = current;
                gate.checkpoint(cancel, bytes, true);
            }});
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
                value->refreshed = value->index->query({}, {}, 0, false).refreshed;
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
    brls::Application::getExitEvent()->subscribe([this] { shutdown(); });
}
Cache::~Cache() { shutdown(); }
void Cache::shutdown() {
    if (exiting) return;
    exiting = true;
    playbackGate.stop(); // wakes a parked or throttled build before joining
    builds.stop();
    queries.stop();
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
    builds.submit([this, job, force, cancel] {
        struct Release {
            std::shared_ptr<State> job;
            PlaybackGate& gate;
            IndexCancel cancel;
            ~Release() { job->refreshing = false; gate.finish(cancel); }
        } release{job, playbackGate, cancel};
        playbackGate.checkpoint(cancel);
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
            if (!buildImdbIndex(job->path, cancel,
                    [this](const std::string& name, const std::string& path, const IndexCancel& cancel) {
                        playbackGate.checkpoint(cancel);
                        downloadDataset(name, path, cancel, playbackGate);
                    }, [job](size_t count) { job->building = count; },
                    [this, cancel] { playbackGate.checkpoint(cancel); })) return;
            auto next = std::make_shared<Snapshot>();
            next->index = std::make_shared<ImdbIndex>(job->path);
            next->refreshed = next->index->query({}, {}, 0, false).refreshed;
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
uint64_t Cache::beginPlayback() {
    playbackMode = PlaybackGate::Mode::Parked;
    ps4diag::write("archive playback mode=parked reason=stream-opening");
    const auto session = playbackGate.beginPlayback();
    gmca::backgroundGovernor().begin(session);
    return session;
}
void Cache::waitForPlayback() { playbackGate.wait(); }
void Cache::playbackState(uint64_t session, bool healthy) {
    playbackGate.playbackState(session, healthy);
    gmca::backgroundGovernor().update(session, healthy);
    const auto after = playbackGate.mode();
    if (playbackMode != after)
        ps4diag::write(after == PlaybackGate::Mode::Background
            ? "archive playback mode=background cpu-duty=5-percent download-kib-s=256"
            : "archive playback mode=parked reason=playback-busy");
    playbackMode = after;
}
void Cache::endPlayback(uint64_t session) {
    playbackGate.endPlayback(session);
    gmca::backgroundGovernor().end(session);
    playbackMode = playbackGate.mode();
    if (playbackMode == PlaybackGate::Mode::Foreground)
        ps4diag::write("archive playback mode=foreground");
}
void Cache::query(const Filter& filter, const Cursor& cursor, size_t limit, bool random,
        std::function<void(Result)> callback, std::shared_ptr<const Snapshot> snapshot) {
    auto job = current();
    const bool interactive = limit || random;
    if (interactive) ps4diag::write("archive query queued");
    const auto queued = std::chrono::steady_clock::now();

    auto execute = [job, filter, cursor, limit, random, callback, snapshot, queued, interactive](
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
                auto found = data->index->query(filter, cursor, limit, random, cancel);
                sqlMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - sqlStarted).count();
                measuringSql = false;
                result.cursor = found.cursor;
                if (interactive) {
                    const auto& m = found.metrics;
                    ps4diag::write("archive query generation=" + std::to_string(found.cursor.generation) +
                        " filter-ms=" + std::to_string(m.filterMs) + " search-index-ms=" + std::to_string(m.searchMs) +
                        " sort-scan-ms=" + std::to_string(m.sortMs) + " metadata-ms=" + std::to_string(m.metadataMs) +
                        " total-ms=" + std::to_string(m.totalMs) + " matches=" + std::to_string(found.total) +
                        " items=" + std::to_string(found.records.size()) + " cursor-in=" + std::to_string(cursor.position) +
                        " cursor-out=" + std::to_string(found.cursor.position) + " reused=" + std::to_string(m.reusedMatches) +
                        " facet-words=" + std::to_string(m.facetWords) + " search-postings=" + std::to_string(m.searchPostings) +
                        " sort-entries=" + std::to_string(m.sortEntries));
                }
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
