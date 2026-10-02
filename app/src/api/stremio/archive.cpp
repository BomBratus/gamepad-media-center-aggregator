#include "api/stremio/archive.hpp"
#include "api/stremio/archive_storage.hpp"
#include "api/stremio/types.hpp"
#include "utils/config.hpp"
#include "utils/thread.hpp"
#include <borealis/core/thread.hpp>
#include <borealis/core/application.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <thread>

namespace stremio::archive {
namespace {
int64_t now() { return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); }

std::string cacheKey(const std::string& account, const std::vector<std::string>& transports) {
    // Stable across launches/platforms; no credentials appear in filenames/logs.
    uint64_t hash = 14695981039346656037ULL;
    auto add = [&](const std::string& value) {
        for (unsigned char c : value) { hash ^= c; hash *= 1099511628211ULL; }
        hash ^= 0; hash *= 1099511628211ULL;
    };
    add(account);
    for (const auto& transport : transports) add(transport);
    return std::to_string(hash);
}

Json fetch(const std::string& url, const HTTP::Cancel& cancel) {
    auto body = HTTP::get(url, HTTP::Timeout{10000, 3000}, cancel);
    if (body.empty()) throw std::runtime_error("Empty catalog response");
    return Json::parse(body);
}

Json compact(Json meta, const std::string& type) {
    Json result = Json::object();
    for (auto key : {"id", "type", "name", "poster", "releaseInfo", "year", "released", "imdbRating", "rating",
                    "genres", "country", "countries", "services", "service", "views", "description", "links"})
        if (meta.contains(key)) result[key] = meta[key];
    if (media::jstr(result, "type").empty()) result["type"] = type;
    auto description = media::jstr(result, "description");
    if (description.size() > 512) {
        size_t end = 512;
        while (end > 0 && (static_cast<unsigned char>(description[end]) & 0xc0) == 0x80) --end;
        result["description"] = description.substr(0, end);
    }
    return result;
}
} // namespace

struct Cache::State {
    std::string key, path;
    std::vector<std::string> transports;
    std::once_flag loaded;
    std::mutex mutex;
    std::shared_ptr<const Snapshot> snapshot = std::make_shared<Snapshot>();
    std::atomic_bool refreshing{false};
    std::atomic<int64_t> nextCheck{0};
    HTTP::Cancel cancelled = std::make_shared<std::atomic_bool>(false);
    int64_t attempted = 0;
    std::string error;

    void load() {
        std::call_once(loaded, [this] {
            try {
                auto value = std::make_shared<Snapshot>(readSnapshot(path));
                std::lock_guard<std::mutex> guard(mutex);
                snapshot = std::move(value);
            } catch (...) {
                std::lock_guard<std::mutex> guard(mutex);
                error = "main/archive/cache_error";
            }
        });
    }
};

Cache& Cache::instance() { static Cache cache; return cache; }

Cache::Cache() {
    brls::Application::getExitEvent()->subscribe([this] {
        if (state) state->cancelled->store(true);
    });
}

std::shared_ptr<Cache::State> Cache::current() {
    auto& config = AppConfig::instance();
    auto transports = config.getStremioAddons();
    auto key = cacheKey(config.getUser().server_id + ":" + config.getUser().id, transports);
    if (!state || state->key != key) {
        if (state) state->cancelled->store(true);
        state = std::make_shared<State>();
        state->key = key;
        state->path = config.configDir() + "/archive-" + key + ".json";
        state->transports = std::move(transports);
    }
    return state;
}

void Cache::refresh(bool force) {
    auto job = current();
    if (!force && now() < job->nextCheck) return;
    if (job->refreshing.exchange(true)) return; // one refresh per account/source set
    // Borealis has a single async queue. A crawl must not occupy it and delay
    // playback/detail requests. Use one existing HTTP-pool slot, serially.
    ThreadPool::instance().submit([job, force](HTTP&) {
        struct Release { std::shared_ptr<State> job; ~Release() { job->refreshing = false; } } release{job};
        if (job->cancelled->load()) return;
        job->load();
        std::shared_ptr<const Snapshot> old;
        {
            std::lock_guard<std::mutex> guard(job->mutex);
            old = job->snapshot;
            const auto time = now();
            if (!force && ((old->refreshed && time - old->refreshed < 3 * 86400) || time - job->attempted < 600)) {
                job->nextCheck = old->refreshed && time - old->refreshed < 3 * 86400 ? old->refreshed + 3 * 86400 : job->attempted + 600;
                job->refreshing = false;
                return;
            }
            job->attempted = time;
            job->nextCheck = time + 600;
            job->error.clear();
        }
        try {
            Snapshot next = *old;
            std::unordered_map<std::string, size_t> ids;
            for (size_t i = 0; i < next.records.size(); ++i) ids[identity(next.records[i].meta)] = i;
            bool partial = false, received = false, failed = false;
            size_t requests = 0;
            const auto started = std::chrono::steady_clock::now();
            auto checkpointAt = started;
            size_t checkpointSize = next.records.size();
            for (const auto& transport : job->transports) {
                if (job->cancelled->load()) return;
                try {
                    auto manifest = parseManifest(fetch(transport, job->cancelled));
                    for (const auto& catalog : manifest.catalogs) {
                        if (!catalog.browsable || (catalog.type != "movie" && catalog.type != "series")) continue;
                        try {
                        size_t skip = 0;
                        std::set<std::string> seen;
                        for (;;) {
                            if (job->cancelled->load()) return;
                            if (++requests > 2000 || std::chrono::steady_clock::now() - started > std::chrono::minutes(30)) {
                                partial = true; break;
                            }
                            std::string url = baseFromTransport(transport) + "/catalog/" + catalog.type + "/" + encodeURIComponent(catalog.id);
                            if (skip) url += "/skip=" + std::to_string(skip);
                            url += ".json";
                            auto page = fetch(url, job->cancelled);
                            auto metas = page.find("metas");
                            if (metas == page.end() || !metas->is_array()) throw std::runtime_error("Invalid catalog response");
                            if (metas->empty()) break;
                            size_t discovered = 0;
                            for (const auto& raw : *metas) {
                                if (!raw.is_object() || media::jstr(raw, "id").empty()) continue;
                                auto meta = compact(raw, catalog.type);
                                auto id = identity(meta);
                                if (!seen.insert(id).second) continue;
                                ++discovered;
                                auto existing = ids.find(id);
                                if (existing == ids.end()) {
                                    if (next.records.size() >= MAX_RECORDS) { partial = true; continue; }
                                    ids[id] = next.records.size();
                                    next.records.push_back({meta, {manifest.name}, now(), now()});
                                } else {
                                    auto& record = next.records[existing->second];
                                    // Fill missing metadata across addons without allowing a sparse
                                    // preview to erase a previously known country/rating/poster.
                                    Json merged = record.meta;
                                    for (auto it = meta.begin(); it != meta.end(); ++it)
                                        if (!it.value().is_null() && !it.value().empty()) merged[it.key()] = it.value();
                                    if (merged != record.meta) { record.meta = std::move(merged); record.updated = now(); }
                                    if (!contains(record.addons, manifest.name)) record.addons.push_back(manifest.name);
                                }
                                received = true;
                            }
                            if (next.records.size() >= checkpointSize + 1000 ||
                                std::chrono::steady_clock::now() - checkpointAt > std::chrono::seconds(60)) {
                                // Persist progress so an interrupted first download remains useful.
                                // Keep the old refresh date until the whole pass finishes.
                                next.partial = true;
                                writeSnapshot(job->path, next);
                                {
                                    std::lock_guard<std::mutex> guard(job->mutex);
                                    job->snapshot = std::make_shared<Snapshot>(next);
                                }
                                checkpointSize = next.records.size();
                                checkpointAt = std::chrono::steady_clock::now();
                            }
                            // Misbehaving addons sometimes ignore skip; never loop forever.
                            if (!discovered) { partial = true; break; }
                            if (page.contains("hasMore") && !media::jbool(page, "hasMore")) break;
                            if (!catalog.hasSkip()) { partial = true; break; }
                            skip += metas->size(); // server page size, not our grid's page size
                            std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        }
                        } catch (...) { partial = true; failed = true; }
                    }
                } catch (...) { partial = true; failed = true; }
                if (requests > 2000 || std::chrono::steady_clock::now() - started > std::chrono::minutes(30)) break;
            }
            if (!received) throw std::runtime_error("No catalogs available");
            if (job->cancelled->load()) return;
            next.partial = partial;
            next.refreshed = failed ? old->refreshed : now();
            writeSnapshot(job->path, next);
            std::lock_guard<std::mutex> guard(job->mutex);
            job->snapshot = std::make_shared<Snapshot>(std::move(next));
            if (!failed) job->nextCheck = now() + 3 * 86400;
            if (partial) job->error = "main/archive/partial";
        } catch (...) {
            std::lock_guard<std::mutex> guard(job->mutex);
            job->error = "main/archive/refresh_error";
        }
        job->refreshing = false;
    });
}

void Cache::query(const Filter& filter, size_t offset, size_t limit, bool random, std::function<void(Result)> callback) {
    auto job = current();
    brls::async([job, filter, offset, limit, random, callback] {
        Result result;
        try {
            job->load();
            std::shared_ptr<const Snapshot> data;
            {
                std::lock_guard<std::mutex> guard(job->mutex);
                data = job->snapshot;
                result.error = job->error;
            }
            result.indexed = data->records.size(); result.refreshed = data->refreshed;
            result.partial = data->partial; result.refreshing = job->refreshing;
            // Progress polling must not repeatedly scan/sort the archive.
            if (limit == 0 && !random) {
                brls::sync([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
                return;
            }
            std::set<std::string> gs, cs, as, ss;
            for (const auto& record : data->records) {
                for (const auto& value : genres(record)) if (!value.empty()) gs.insert(value);
                for (const auto& value : countries(record)) if (!value.empty()) cs.insert(value);
                for (const auto& value : record.addons) if (!value.empty()) as.insert(value);
                for (const auto& value : services(record)) if (!value.empty()) ss.insert(value);
                if (media::jint(record.meta, "views", -1) >= 0) result.options.hasViews = true;
            }
            result.options.genres.assign(gs.begin(), gs.end()); result.options.countries.assign(cs.begin(), cs.end());
            result.options.addons.assign(as.begin(), as.end());
            result.options.services.assign(ss.begin(), ss.end());
            auto indices = select(data->records, filter, !random);
            result.total = indices.size();
            if (random) {
                static std::mt19937 engine(static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
                auto picked = randomMatch(indices, engine);
                if (picked != size_t(-1)) result.items.push_back(parseMetaPreview(data->records[picked].meta));
            } else {
                for (size_t i = std::min(offset, indices.size()); i < indices.size() && result.items.size() < limit; ++i)
                    result.items.push_back(parseMetaPreview(data->records[indices[i]].meta));
            }
        } catch (...) { result.error = "main/archive/cache_error"; }
        brls::sync([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
    });
}
} // namespace stremio::archive
