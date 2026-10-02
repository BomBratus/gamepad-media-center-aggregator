/* GMCA media models and playback. Persisted field names remain compatible. */

#include "api/stremio/backend.hpp"
#include "api/stremio/types.hpp"
#include "api/stremio/catalog_navigation.hpp"
#include "api/stremio/catalog_aggregation.hpp"
#include "api/stremio/episode_continuation.hpp"
#include "api/stremio/playback_completion.hpp"
#include "api/stremio/playback_resume.hpp"
#include "api/stremio/subtitle_identity.hpp"
#include "api/stremio/async_playback_history.hpp"
#include "api/stremio/auth.hpp"
#include "api/media/langs.hpp"
#include "utils/config.hpp"
#include "utils/misc.hpp"
#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/core/i18n.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

using namespace brls::literals;  // continue-watching hub title localized like the rest of the UI

namespace stremio {

namespace {

/// Map a UI MediaKind to the Stremio resource types it can match. Empty = any.
std::set<std::string> kindToStremioTypes(media::MediaKind k) {
    switch (k) {
        case media::MediaKind::Movie:
            return {"movie"};
        case media::MediaKind::Show:
            return {"series", "anime"};
        default:
            return {};  // Any
    }
}

/// Emit an empty Container<T> via `then` without touching the network. Used for
/// the navigation verbs Stremio has no concept of (collections, playlists, …).
template <typename T>
void emptyContainer(media::Then<media::Container<T>> then) {
    if (then) brls::sync([then]() { then(media::Container<T>{}); });
}

// ---- account datastore helpers (library = watchlist + playback state) ----------

/// authKey of the connected account (empty when navigating without one).
std::string accountKey() { return AppConfig::instance().getToken(); }

/// Stremio's account library keeps playback state at the SERIES level. Its
/// per-episode `state.watched` field is a compressed bitmap whose write format is
/// not part of the addon protocol. Keep GMCA's episode history and fine-grained
/// resume positions in small local JSON files, scoped to the active account.
std::string watchedHistoryPath() { return AppConfig::instance().configDir() + "/stremio-watched.json"; }
std::string progressHistoryPath() { return AppConfig::instance().configDir() + "/stremio-progress.json"; }
std::string playbackHistoryPath() { return AppConfig::instance().configDir() + "/stremio-playback.json"; }

AsyncPlaybackHistory& playbackHistory() {
    static AsyncPlaybackHistory history(playbackHistoryPath(), [] {
        brls::Logger::warning("stremio playback checkpoint write failed");
    });
    return history;
}

std::string playbackScope() {
    if (accountKey().empty()) return {};
    const auto& config = AppConfig::instance();
    const auto& user = config.getUser();
    return nlohmann::json::array({user.server_id, config.getUserId()}).dump();
}

void clearSavedPlayback(const std::string& id) {
    const std::string scope = playbackScope();
    if (scope.empty()) return;
    playbackHistory().clearAsync(scope, id);
}

std::string watchedHistoryScope() {
    const auto& user = AppConfig::instance().getUser();
    if (!user.server_id.empty()) return user.server_id;
    return AppConfig::instance().getUserId();
}

bool readJsonObject(const std::string& path, nlohmann::json& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    nlohmann::json j;
    in >> j;
    if (!j.is_object()) throw std::runtime_error("root is not an object");
    out = std::move(j);
    return true;
}

nlohmann::json loadJsonObjectWithBackup(const std::string& path) {
    nlohmann::json root;
    try {
        if (readJsonObject(path, root)) return root;
    } catch (const std::exception& ex) {
        brls::Logger::warning("stremio local state read {}: {}", path, ex.what());
    }

    try {
        if (readJsonObject(path + ".bak", root)) {
            brls::Logger::warning("stremio local state recovered from {}", path + ".bak");
            return root;
        }
    } catch (const std::exception& ex) {
        brls::Logger::warning("stremio local state backup read {}: {}", path, ex.what());
    }
    return nlohmann::json::object();
}

bool jsonObjectFileIsValid(const std::string& path) {
    try {
        nlohmann::json ignored;
        return readJsonObject(path, ignored);
    } catch (...) {
        return false;
    }
}

bool writeJsonObjectAtomic(const std::string& path, const nlohmann::json& root) {
    const std::string tmp = path + ".tmp";
    const std::string bak = path + ".bak";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out.is_open()) return false;
        out << root.dump(2);
        out.flush();
        if (!out.good()) {
            out.close();
            std::remove(tmp.c_str());
            return false;
        }
        out.close();
        if (!out.good()) {
            std::remove(tmp.c_str());
            return false;
        }
    }

    const bool keepCurrent = jsonObjectFileIsValid(path);
    if (keepCurrent) {
        std::remove(bak.c_str());
        if (std::rename(path.c_str(), bak.c_str()) != 0) {
            std::remove(tmp.c_str());
            return false;
        }
    } else {
        // Never replace a known-good backup with a corrupt/truncated primary.
        std::remove(path.c_str());
    }

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        if (keepCurrent) std::rename(bak.c_str(), path.c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::mutex watchedHistoryMutex;
std::mutex completionMutex;
PlaybackCompletion completion;

std::set<std::string> loadWatchedEpisodes() {
    playbackHistory().flush();
    std::lock_guard<std::mutex> lock(watchedHistoryMutex);
    std::set<std::string> out;
    std::string scope = watchedHistoryScope();
    if (scope.empty()) return out;
    nlohmann::json root = loadJsonObjectWithBackup(watchedHistoryPath());
    auto it = root.find(scope);
    if (it == root.end() || !it->is_array()) return out;
    for (auto& v : *it)
        if (v.is_string()) out.insert(v.get<std::string>());
    return out;
}

void setEpisodeWatched(const std::string& ratingKey, bool watched) {
    const std::string scope = watchedHistoryScope(), path = watchedHistoryPath();
    if (scope.empty()) return;
    playbackHistory().enqueue([scope, path, ratingKey, watched]() {
        std::lock_guard<std::mutex> lock(watchedHistoryMutex);
        nlohmann::json root = loadJsonObjectWithBackup(path);
        std::set<std::string> items;
        auto current = root.find(scope);
        if (current != root.end() && current->is_array())
            for (auto& v : *current)
                if (v.is_string()) items.insert(v.get<std::string>());
        if (watched)
            items.insert(ratingKey);
        else
            items.erase(ratingKey);
        root[scope] = nlohmann::json::array();
        for (auto& id : items) root[scope].push_back(id);
        if (!writeJsonObjectAtomic(path, root))
            brls::Logger::warning("stremio watched history: atomic write failed");
    });
}

void applyEpisodeWatched(media::Item& item, const std::set<std::string>& watched) {
    if (item.type == media::mediaTypeEpisode && watched.count(item.ratingKey) > 0) {
        item.viewCount = 1;
        item.viewOffset = 0;
    }
}

struct EpisodeProgress {
    std::string baseId;
    std::string videoId;
    int64_t timeOffset = 0;
    int64_t duration = 0;
    int64_t updated = 0;
    std::string updatedIso;
};

std::mutex episodeProgressMutex;
std::string episodeProgressAccount;
std::map<std::string, EpisodeProgress> episodeProgressCache;
std::mutex localProgressMutex;
std::mutex progressSyncMutex;
std::mutex libraryWriteMutex;
std::map<std::string, int64_t> lastProgressSync;
std::map<std::string, int64_t> progressSyncGeneration;

void cacheEpisodeProgresses(const std::string& account, const nlohmann::json& items) {
    std::lock_guard<std::mutex> lock(episodeProgressMutex);
    if (episodeProgressAccount != account) {
        episodeProgressAccount = account;
    }
    episodeProgressCache.clear();  // datastoreGet returns the complete account library
    for (auto& item : items) {
        if (jstr(item, "type") != "series" && jstr(item, "type") != "anime") continue;
        auto state = item.find("state");
        if (state == item.end() || !state->is_object()) continue;
        EpisodeProgress progress;
        progress.baseId = jstr(item, "_id");
        progress.videoId = jstr(*state, "videoId");
        progress.timeOffset = jint(*state, "timeOffset");
        progress.duration = jint(*state, "duration");
        progress.updatedIso = jstr(*state, "lastWatched");
        episodeProgressCache[progress.baseId] = std::move(progress);
    }
}

void setEpisodeProgress(const std::string& account, const std::string& showId, EpisodeProgress progress) {
    if (account.empty()) return;
    std::lock_guard<std::mutex> lock(episodeProgressMutex);
    if (episodeProgressAccount != account) {
        episodeProgressAccount = account;
        episodeProgressCache.clear();
    }
    progress.baseId = showId;
    episodeProgressCache[showId] = std::move(progress);
}

EpisodeProgress loadEpisodeProgress(const std::string& showId) {
    std::string account = accountKey();
    if (account.empty()) return {};
    std::lock_guard<std::mutex> lock(episodeProgressMutex);
    if (episodeProgressAccount != account) return {};
    auto it = episodeProgressCache.find(showId);
    return it == episodeProgressCache.end() ? EpisodeProgress{} : it->second;
}

std::map<std::string, EpisodeProgress> loadLocalProgresses() {
    playbackHistory().flush();
    std::map<std::string, EpisodeProgress> out;
    std::string scope = watchedHistoryScope();
    if (scope.empty()) return out;
    {
        std::lock_guard<std::mutex> lock(localProgressMutex);
        nlohmann::json root = loadJsonObjectWithBackup(progressHistoryPath());
        auto scoped = root.find(scope);
        if (scoped != root.end() && scoped->is_object()) {
            for (auto it = scoped->begin(); it != scoped->end(); ++it) {
                if (!it.value().is_object()) continue;
                EpisodeProgress p;
                p.baseId = jstr(it.value(), "baseId");
                p.videoId = jstr(it.value(), "videoId");
                p.timeOffset = jint(it.value(), "timeOffset");
                p.duration = jint(it.value(), "duration");
                p.updated = jint(it.value(), "updated");
                if (!p.baseId.empty() && p.timeOffset > 0) {
                    p.updatedIso = PlaybackHistory::timestampIso(p.updated * 1000);
                    out[it.key()] = std::move(p);
                }
            }
        }

    } // Release the legacy mutex before waiting on the history writer.
    const std::string localScope = playbackScope();
    if (!localScope.empty()) {
        const nlohmann::json records = playbackHistory().records(localScope);
        for (auto it = records.begin(); it != records.end(); ++it) {
            if (!it.value().is_object()) continue;
            ParsedId pid = parseId(it.key());
            EpisodeProgress p;
            p.baseId = pid.baseId;
            p.videoId = (pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0 ? pid.stremioId : "";
            p.timeOffset = jint(it.value(), "position");
            p.duration = jint(it.value(), "duration");
            const int64_t updatedMs = jint(it.value(), "updated");
            p.updated = updatedMs / 1000;
            p.updatedIso = PlaybackHistory::updatedIso(it.value());
            if (!p.baseId.empty()) out[it.key()] = std::move(p);  // zero is an explicit clear
        }
    }
    return out;
}

void storeLocalProgress(const std::string& ratingKey, const EpisodeProgress& progress) {
    const std::string scope = watchedHistoryScope(), path = progressHistoryPath();
    if (scope.empty()) return;
    playbackHistory().enqueue([scope, path, ratingKey, progress]() {
        std::lock_guard<std::mutex> lock(localProgressMutex);
        nlohmann::json root = loadJsonObjectWithBackup(path);
        if (!root.contains(scope) || !root[scope].is_object()) root[scope] = nlohmann::json::object();
        if (progress.timeOffset <= 0) {
            root[scope].erase(ratingKey);
        } else {
            root[scope][ratingKey] = {{"baseId", progress.baseId}, {"videoId", progress.videoId},
                {"timeOffset", progress.timeOffset}, {"duration", progress.duration}, {"updated", progress.updated}};
        }
        if (!writeJsonObjectAtomic(path, root))
            brls::Logger::warning("stremio progress history: atomic write failed");
    });
}

// The caller serializes this transition with checkpoint enqueueing.
void queueLocalCompletion(const std::string& ratingKey, const ParsedId& id, int64_t duration, int64_t now) {
    const bool episode = (id.stremioType == "series" || id.stremioType == "anime") && id.episode >= 0;
    storeLocalProgress(ratingKey, {id.baseId, episode ? id.stremioId : "", 0, duration, now});
    if (episode) setEpisodeWatched(ratingKey, true);
    clearSavedPlayback(ratingKey);
}

const EpisodeProgress* newestLocalProgress(
    const std::map<std::string, EpisodeProgress>& local, const std::string& baseId, bool series) {
    const EpisodeProgress* best = nullptr;
    for (const auto& kv : local) {
        const auto& p = kv.second;
        if (p.baseId != baseId || (series && p.videoId.empty()) || (!series && !p.videoId.empty())) continue;
        if (!best || p.updated > best->updated || (p.updated == best->updated && p.updatedIso > best->updatedIso))
            best = &p;
    }
    return best;
}

bool localAtLeastAsNew(const EpisodeProgress& local, const std::string& remoteTimestamp) {
    const nlohmann::json localSeconds = {{"updated", local.updated * 1000}};
    if (PlaybackHistory::localIsNewer(localSeconds, remoteTimestamp)) return true;

    const std::string localIso = !local.updatedIso.empty()
        ? local.updatedIso
        : PlaybackHistory::timestampIso(local.updated * 1000);
    std::string normalizedRemote = remoteTimestamp;
    if (normalizedRemote.size() == 20 && normalizedRemote.back() == 'Z')
        normalizedRemote = normalizedRemote.substr(0, 19) + ".000Z";
    return !localIso.empty() && localIso >= normalizedRemote;
}

int64_t reserveRemoteProgressSync(
    const std::string& account, const std::string& ratingKey, media::PlayState state, int64_t now) {
    std::lock_guard<std::mutex> lock(progressSyncMutex);
    const std::string key = account + "\n" + ratingKey;
    if (state == media::PlayState::Playing) {
        auto it = lastProgressSync.find(key);
        if (it != lastProgressSync.end() && now - it->second < 60) return 0;
        lastProgressSync[key] = now;
    } else if (state != media::PlayState::Paused && state != media::PlayState::Stopped) {
        return 0;
    }
    const std::string resource = nlohmann::json::array({account, playbackResourceKey(ratingKey)}).dump();
    return ++progressSyncGeneration[resource];
}

bool isLatestRemoteProgressSync(
    const std::string& account, const std::string& ratingKey, int64_t generation) {
    std::lock_guard<std::mutex> lock(progressSyncMutex);
    const std::string key = nlohmann::json::array({account, playbackResourceKey(ratingKey)}).dump();
    auto it = progressSyncGeneration.find(key);
    return it != progressSyncGeneration.end() && it->second == generation;
}

void applyEpisodeProgress(media::Item& item, const EpisodeProgress& remote,
    const std::map<std::string, EpisodeProgress>& local, const std::set<std::string>& watched) {
    auto localIt = local.find(item.ratingKey);
    if (localIt != local.end()) {
        const auto& p = localIt->second;
        if (p.timeOffset == 0 && localAtLeastAsNew(p, remote.updatedIso)) {
            item.viewOffset = 0;
            if (p.duration > 0) item.duration = p.duration;
            return;
        }
        if (p.timeOffset > 0 && (p.duration <= 0 || p.timeOffset < p.duration)) {
            if (p.videoId != remote.videoId || localAtLeastAsNew(p, remote.updatedIso)) {
                applyContinuationCheckpoint(item, p.timeOffset, p.duration);
                return;
            }
        }
    }

    if (item.guid != remote.videoId || remote.timeOffset <= 0 || watched.count(item.ratingKey) > 0)
        return;
    applyContinuationCheckpoint(item, remote.timeOffset, remote.duration);
}

/// A datastore libraryItem JSON -> media::Item (movie/show row). ratingKey is the
/// opaque "{type}:{_id}"; resume offset/watched come from state.
media::Item itemFromLibrary(const nlohmann::json& j) {
    media::Item it;
    std::string id = jstr(j, "_id");
    std::string type = jstr(j, "type");
    it.ratingKey = type + ":" + id;
    it.key = it.ratingKey;
    it.guid = id;
    it.type = mapType(type);
    it.title = jstr(j, "name");
    it.thumb = jstr(j, "poster");
    auto st = j.find("state");
    if (st != j.end() && st->is_object()) {
        it.viewOffset = jint(*st, "timeOffset");
        it.viewCount = jint(*st, "flaggedWatched") > 0 ? 1 : 0;
        it.duration = jint(*st, "duration");
    }
    return it;
}

void appendHistoryOnlyLibraryRows(nlohmann::json& items, const nlohmann::json& records,
    const std::map<std::string, EpisodeProgress>& local) {
    std::set<std::string> knownBases;
    if (items.is_array())
        for (const auto& item : items) {
            const std::string baseId = jstr(item, "_id");
            if (!baseId.empty()) knownBases.insert(baseId);
        }

    if (!records.is_object()) return;
    for (auto it = records.begin(); it != records.end(); ++it) {
        if (!it.value().is_object()) continue;
        const auto metadataIt = it.value().find("item");
        if (metadataIt == it.value().end() || !metadataIt->is_object()) continue;
        const std::string ratingKey = jstr(*metadataIt, "ratingKey", it.key());
        const ParsedId pid = parseId(ratingKey);
        const bool movie = pid.stremioType == "movie";
        const bool episode = (pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0;
        if ((!movie && !episode) || pid.baseId.empty() || !knownBases.insert(pid.baseId).second) continue;

        std::string name = movie ? jstr(*metadataIt, "title") : jstr(*metadataIt, "grandparentTitle");
        std::string poster = movie ? jstr(*metadataIt, "thumb") : jstr(*metadataIt, "grandparentThumb");
        if (name.empty()) name = jstr(*metadataIt, "title");
        if (poster.empty()) poster = jstr(*metadataIt, "thumb");

        nlohmann::json state = {
            {"timeOffset", jint(it.value(), "position")},
            {"duration", jint(it.value(), "duration")},
            {"flaggedWatched", 0},
        };
        if (episode) state["videoId"] = pid.stremioId;
        auto latest = newestLocalProgress(local, pid.baseId, !movie);
        if (latest) {
            state["timeOffset"] = latest->timeOffset;
            if (latest->duration > 0) state["duration"] = latest->duration;
            if (episode) state["videoId"] = latest->videoId;
            state["lastWatched"] = !latest->updatedIso.empty()
                ? latest->updatedIso
                : PlaybackHistory::timestampIso(latest->updated * 1000);
        } else {
            state["lastWatched"] = PlaybackHistory::updatedIso(it.value());
        }
        items.push_back({
            {"_id", pid.baseId},
            {"name", name},
            {"type", pid.stremioType},
            {"poster", poster},
            {"state", std::move(state)},
        });
    }
}

/// Find-or-create the libraryItem for `ratingKey` (by its base movie/show id),
/// apply `mutate` to its state, and PUT it back. A new entry is built from /meta
/// (name/poster) and defaults to removed+temp (progress only, not in library).
/// Synchronous — call inside a brls::async body. No-op without an account.
void upsertLibrary(AddonEngine& engine, const std::string& key, const std::string& ratingKey,
    const std::function<void(nlohmann::json&)>& mutate, const std::string& reportedAt = "") {
    if (key.empty()) return;
    ParsedId pid = parseId(ratingKey);
    std::string baseId = pid.baseId;
    std::string libType = pid.stremioType == "anime" ? "anime" : (pid.stremioType == "movie" ? "movie" : "series");

    nlohmann::json items = stremio::datastoreGet(key);
    nlohmann::json found;
    for (auto& it : items)
        if (jstr(it, "_id") == baseId) {
            found = it;
            break;
        }

    std::string now = stremio::nowIso();
    const std::string stateTimestamp = reportedAt.empty() ? now : reportedAt;
    if (found.is_null()) {
        std::string name, poster;
        engine.ensureLoaded();
        for (auto& a : engine.addonsFor("meta", libType, baseId)) {
            try {
                nlohmann::json mj = getSync(engine.resourceUrl(a, "meta", libType, baseId));
                auto m = mj.find("meta");
                if (m != mj.end() && m->is_object()) {
                    name = jstr(*m, "name");
                    poster = jstr(*m, "poster");
                    break;
                }
            } catch (...) {
            }
        }
        found = {
            {"_id", baseId}, {"name", name}, {"type", libType}, {"poster", poster}, {"posterShape", "poster"},
            {"removed", true}, {"temp", true}, {"_ctime", now}, {"_mtime", now},
            {"state", {{"lastWatched", now}, {"timeWatched", 0}, {"timeOffset", 0}, {"overallTimeWatched", 0},
                          {"timesWatched", 0}, {"flaggedWatched", 0}, {"duration", 0}, {"videoId", nullptr},
                          {"watched", nullptr}, {"noNotif", false}}},
            {"behaviorHints", nlohmann::json::object()},
        };
    }
    if (!found.contains("state") || !found["state"].is_object()) found["state"] = nlohmann::json::object();
    mutate(found["state"]);
    found["state"]["lastWatched"] = stateTimestamp;
    found["_mtime"] = now;
    stremio::datastorePut(key, found);
}

// ---- catalog routing + localized labels ----------------------------------------

/// Pre-resolved UI strings (i18n lookups must happen on the UI thread, so a verb
/// loads these before going async and captures them into the worker lambda).
struct L10n {
    std::string movies, series, anime, popular, news, featured;
};
inline L10n loadL10n() {
    return {"main/stremio/movies"_i18n, "main/stremio/series"_i18n, "main/stremio/anime/title"_i18n,
        "main/stremio/popular"_i18n, "main/stremio/new"_i18n, "main/stremio/featured"_i18n};
}

/// Localized label for a Stremio content type and for a catalog. Cinemeta's
/// canonical catalogs (top/year/imdbRating) get a translated name; any other
/// addon catalog keeps its declared (server) name.
std::string typeLabel(const L10n& l, const std::string& stremioType) {
    if (stremioType == "movie") return l.movies;
    if (stremioType == "series") return l.series;
    if (stremioType == "anime") return l.anime;
    return stremioType;
}
std::string catalogLabel(const L10n& l, const Catalog& c) {
    if (c.id == "top") return l.popular;
    if (c.id == "year") return l.news;
    if (c.id == "imdbRating") return l.featured;
    return c.name;
}
/// Like catalogLabel but, when the catalog is unnamed (name fell back to its id),
/// use the addon's name instead (e.g. "publicdomainmovies" -> "Public Domain Movies").
std::string bestCatalogLabel(const L10n& l, const Addon& a, const Catalog& c) {
    std::string lbl = catalogLabel(l, c);
    if (lbl == c.id && !a.manifest.name.empty()) return a.manifest.name;
    return lbl;
}

/// A section/hub key is a bare Stremio type ("movie"/"series"), the synthetic
/// "anime" category, or a routed catalog key "base\ttype\tcatalogId[\tgenre]".
bool isCatalogKey(const std::string& s) { return s.find('\t') != std::string::npos; }
std::string catalogKey(
    const std::string& base, const std::string& type, const std::string& id, const std::string& genre = "") {
    return makeCatalogRouteKey({base, type, id, genre});
}
bool splitCatalogKey(const std::string& key, std::string& base, std::string& type, std::string& id,
    std::string* genre = nullptr) {
    CatalogRoute route;
    if (!parseCatalogRouteKey(key, route)) return false;
    base = std::move(route.base);
    type = std::move(route.type);
    id = std::move(route.id);
    if (genre) *genre = std::move(route.genre);
    return true;
}

struct AnimeCatalog {
    Addon addon;
    Catalog catalog;
    std::string genreFilter;
};

std::vector<AnimeCatalog> animeCatalogs(AddonEngine& engine) {
    std::vector<AnimeCatalog> out;
    for (const char* type : {"movie", "series", "anime"}) {
        for (const auto& pc : engine.catalogsForType(type)) {
            AnimeCatalogKind kind = classifyAnimeCatalog(pc.second.type, pc.second.id, pc.second.name,
                pc.second.genres, pc.second.hasGenre());
            if (kind == AnimeCatalogKind::None) continue;
            out.push_back({pc.first, pc.second, kind == AnimeCatalogKind::GenreFiltered ? "Anime" : ""});
        }
    }
    return out;
}

/// Build a catalog resource URL from a base (no Addon object needed).
std::string buildCatalogUrl(const std::string& base, const std::string& type, const std::string& id,
    const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::string url = base + "/catalog/" + type + "/" + encodeURIComponent(id);
    if (!extra.empty()) {
        std::string joined;
        for (size_t i = 0; i < extra.size(); ++i) {
            if (i) joined += "&";
            joined += extra[i].first + "=" + encodeURIComponent(extra[i].second);
        }
        url += "/" + joined;
    }
    url += ".json";
    return url;
}

std::string animeCatalogUrl(const AnimeCatalog& entry, size_t skip = 0) {
    std::vector<std::pair<std::string, std::string>> extra;
    if (skip > 0) extra.emplace_back("skip", std::to_string(skip));
    if (!entry.genreFilter.empty()) extra.emplace_back("genre", entry.genreFilter);
    return buildCatalogUrl(entry.addon.base, entry.catalog.type, entry.catalog.id, extra);
}

// ---- IMDb all-time Top 250 ----------------------------------------------------
// IMDb's public chart pages are the semantic source, but fetching/parsing their
// HTML on a console is brittle. The same chart data is exposed by IMDb's internal
// GraphQL endpoint in one compact request. It is deliberately isolated here in
// the Stremio backend: the UI only sees neutral media::Item rows.
//
// Reliability contract: cache one snapshot per kind on disk and in RAM. A fresh
// (<24 h) snapshot avoids the network entirely; if a refresh fails, stale cached
// data is still shown rather than silently substituting Cinemeta "Featured".
constexpr int64_t IMDB_TOP_CACHE_TTL = 24 * 60 * 60;
constexpr int IMDB_TOP_LIMIT = 250;

struct ImdbTopCacheState {
    std::mutex mutex;
    std::vector<media::Item> movies;
    std::vector<media::Item> series;
    int64_t moviesFetchedAt = 0;
    int64_t seriesFetchedAt = 0;
    bool moviesLoaded = false;
    bool seriesLoaded = false;
};
ImdbTopCacheState imdbTopCache;

std::string imdbTopCachePath(bool series) {
    return AppConfig::instance().configDir() + (series ? "/imdb-top-series.json" : "/imdb-top-movies.json");
}

media::Item imdbTopItemFromJson(const nlohmann::json& j, bool series) {
    media::Item item;
    std::string id = jstr(j, "id");
    if (id.rfind("tt", 0) != 0) return item;
    item.guid = id;
    item.ratingKey = std::string(series ? "series:" : "movie:") + id;
    item.key = item.ratingKey;
    item.type = series ? media::mediaTypeShow : media::mediaTypeMovie;
    item.title = jstr(j, "title");
    item.year = jint(j, "year");
    item.rating = jnum(j, "rating");
    item.ratingImage = "imdb://image.rating";
    item.index = jint(j, "rank");  // rank is otherwise unused for movie/show cards
    item.thumb = "https://images.metahub.space/poster/medium/" + id + "/img";
    return item;
}

void loadImdbTopCacheFile(bool series, std::vector<media::Item>& out, int64_t& fetchedAt) {
    out.clear();
    fetchedAt = 0;
    try {
        std::ifstream in(imdbTopCachePath(series));
        if (!in.is_open()) return;
        nlohmann::json root;
        in >> root;
        if (jint(root, "schema") != 1) return;
        fetchedAt = jint(root, "fetchedAt");
        auto it = root.find("items");
        if (it == root.end() || !it->is_array()) return;
        for (const auto& entry : *it) {
            media::Item item = imdbTopItemFromJson(entry, series);
            if (!item.ratingKey.empty()) out.push_back(std::move(item));
        }
    } catch (const std::exception& ex) {
        brls::Logger::warning("IMDb Top cache read {}: {}", imdbTopCachePath(series), ex.what());
        out.clear();
        fetchedAt = 0;
    }
}

void saveImdbTopCacheFile(bool series, const std::vector<media::Item>& items, int64_t fetchedAt) {
    try {
        nlohmann::json root;
        root["schema"] = 1;
        root["fetchedAt"] = fetchedAt;
        root["items"] = nlohmann::json::array();
        for (const auto& item : items) {
            root["items"].push_back({{"id", item.guid}, {"title", item.title}, {"year", item.year},
                {"rating", item.rating}, {"rank", item.index}});
        }
        std::ofstream out(imdbTopCachePath(series), std::ios::trunc);
        if (!out.is_open()) throw std::runtime_error("could not open cache file");
        out << root.dump();
    } catch (const std::exception& ex) {
        brls::Logger::warning("IMDb Top cache write {}: {}", imdbTopCachePath(series), ex.what());
    }
}

std::vector<media::Item> fetchImdbTop(media::MediaKind kind) {
    const bool series = kind == media::MediaKind::Show;
    const std::string chartType = series ? "TOP_RATED_TV_SHOWS" : "TOP_RATED_MOVIES";
    const std::string query =
        "query GetChart($first: Int!) {"
        " chartTitles(first: $first, chart: {chartType: " + chartType + "}) {"
        " edges { node { id titleText { text } releaseYear { year }"
        " ratingsSummary { aggregateRating voteCount topRanking { rank } } } }"
        " }"
        "}";
    nlohmann::json body = {
        {"query", query}, {"variables", {{"first", IMDB_TOP_LIMIT}}}, {"operationName", "GetChart"}};
    HTTP::Header headers = {
        "Content-Type: application/json",
        "Accept: application/json",
        "Origin: https://www.imdb.com",
        "Referer: https://www.imdb.com/",
        "User-Agent: Mozilla/5.0 (GMCA; IMDb Top 250)",
    };
    HTTP::Timeout timeout;
    timeout.timeout = 10000;
    timeout.connect = 3500;
    nlohmann::json root = nlohmann::json::parse(
        HTTP::post("https://api.graphql.imdb.com/", body.dump(), headers, timeout));
    if (root.contains("errors")) throw std::runtime_error("IMDb chart request returned an error");
    auto data = root.find("data");
    if (data == root.end() || !data->is_object()) throw std::runtime_error("IMDb chart response has no data");
    auto chart = data->find("chartTitles");
    if (chart == data->end() || !chart->is_object()) throw std::runtime_error("IMDb chart response has no chart");
    auto edges = chart->find("edges");
    if (edges == chart->end() || !edges->is_array()) throw std::runtime_error("IMDb chart response has no rows");

    std::vector<media::Item> out;
    out.reserve(IMDB_TOP_LIMIT);
    int64_t fallbackRank = 1;
    for (const auto& edge : *edges) {
        auto node = edge.find("node");
        if (node == edge.end() || !node->is_object()) continue;
        std::string id = jstr(*node, "id");
        if (id.rfind("tt", 0) != 0) continue;

        media::Item item;
        item.guid = id;
        item.ratingKey = std::string(series ? "series:" : "movie:") + id;
        item.key = item.ratingKey;
        item.type = series ? media::mediaTypeShow : media::mediaTypeMovie;
        if (auto title = node->find("titleText"); title != node->end() && title->is_object())
            item.title = jstr(*title, "text");
        if (auto year = node->find("releaseYear"); year != node->end() && year->is_object())
            item.year = jint(*year, "year");
        item.index = fallbackRank;
        if (auto ratings = node->find("ratingsSummary"); ratings != node->end() && ratings->is_object()) {
            item.rating = jnum(*ratings, "aggregateRating");
            item.ratingImage = "imdb://image.rating";
            if (auto top = ratings->find("topRanking"); top != ratings->end() && top->is_object()) {
                int64_t rank = jint(*top, "rank");
                if (rank > 0) item.index = rank;
            }
        }
        item.thumb = "https://images.metahub.space/poster/medium/" + id + "/img";
        out.push_back(std::move(item));
        ++fallbackRank;
    }
    if (out.empty()) throw std::runtime_error("IMDb chart returned no titles");
    std::stable_sort(out.begin(), out.end(), [](const media::Item& a, const media::Item& b) {
        return a.index < b.index;
    });
    return out;
}

media::Container<media::Item> pageImdbTop(
    const std::vector<media::Item>& items, size_t start, size_t size) {
    media::Container<media::Item> out;
    out.StartIndex = (long)start;
    out.TotalRecordCount = (long)items.size();
    if (start >= items.size() || size == 0) return out;
    size_t end = std::min(items.size(), start + size);
    out.Items.insert(out.Items.end(), items.begin() + start, items.begin() + end);
    return out;
}

media::Stream subtitleOptionToStream(const SubtitleOption& sub) {
    media::Stream st;
    st.streamType = media::streamTypeSubtitle;
    st.key = sub.url;
    st.language = sub.lang;
    st.languageTag = media::subtitleLangCode(sub.lang);
    st.displayTitle = media::subtitleLangDisplay(sub.lang);
    if (st.displayTitle.empty()) st.displayTitle = "Subtitle";
    return st;
}

/// Fan out /stream across the addons serving (type,id) and return EVERY source
/// as a neutral media::Media row (parsed quality/codec/size/kind/cache), ordered
/// playable-first by quality (cached debrid before uncached), then the non-
/// playable sources (torrent/external/youtube) by quality. The order is stable
/// so the index the detail page shows matches the one PlayerView re-resolves at
/// play time. Debrid addons resolve infoHash -> a real url server-side, landing
/// here as a playable Direct/Debrid row; infoHash-only/ytId/externalUrl stay
/// non-playable (no local torrent client / browser on console).
std::vector<media::Media> resolveAllStreams(
    AddonEngine& engine, const std::string& stremioType, const std::string& stremioId,
    const std::string& savedIdentity = "", bool* complete = nullptr,
    std::vector<media::Media> known = {}) {
    std::vector<media::Media> all = std::move(known);
    std::set<std::string> knownRequests;
    for (const auto& source : all) {
        try {
            const auto request = savedProviderRequest(source.sourceIdentity);
            if (!request.empty()) knownRequests.insert(request);
        } catch (...) {}
    }
    if (complete) *complete = true;
    auto providers = engine.addonsFor("stream", stremioType, stremioId);
    std::map<std::string, size_t> providerOrder;
    for (size_t i = 0; i < providers.size(); ++i)
        providerOrder[engine.resourceUrl(providers[i], "stream", stremioType, stremioId)] = i;
    const std::string preferredRequest = savedProviderRequest(savedIdentity);
    if (!preferredRequest.empty()) {
        std::stable_partition(providers.begin(), providers.end(), [&](const Addon& addon) {
            return engine.resourceUrl(addon, "stream", stremioType, stremioId) == preferredRequest;
        });
    }
    for (auto& a : providers) {
        std::string url = engine.resourceUrl(a, "stream", stremioType, stremioId);
        if (knownRequests.count(url)) continue;
        std::vector<StreamOption> streams;
        try {
            streams = parseStreams(getSync(url, streamRequestTimeout()));
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio stream {}: {}", redactUrlForLog(url), ex.what());
            continue;
        }
        for (size_t release = 0; release < streams.size(); ++release) {
            const auto& s = streams[release];
            media::Media media = streamToMedia(s, a.manifest.name);
            media.sourceProviderOrder = providerOrder[url];
            media.sourceReleaseOrder = release;
            // Scope release identity to the exact provider request so two
            // addons with the same display name cannot collide in playback history.
            if (!media.sourceIdentity.empty())
                media.sourceIdentity = nlohmann::json::array({url, media.sourceIdentity}).dump();
            // Stremio streams may carry release-specific subtitle sidecars.
            // Keep them on the chosen Part so PlayerView's existing MPV_LOADED
            // path can sub-add them without another network round-trip.
            if (!media.parts.empty()) {
                for (const auto& sub : s.subtitles) {
                    if (sub.url.empty()) continue;
                    media.parts.front().streams.push_back(subtitleOptionToStream(sub));
                }
            }
            all.push_back(std::move(media));
        }
        if (url == preferredRequest) {
            const auto matches = std::count_if(all.begin(), all.end(), [&](const media::Media& source) {
                return source.playable() && source.sourceIdentity == savedIdentity;
            });
            auto match = std::find_if(all.begin(), all.end(), [&](const media::Media& source) {
                return source.playable() && source.sourceIdentity == savedIdentity;
            });
            if (matches == 1 && match != all.end()) {
                std::rotate(all.begin(), match, match + 1);
                if (complete) *complete = providers.size() == 1;
                return all;
            }
        }
    }
    restoreAddonSourceOrder(all);
    std::stable_sort(all.begin(), all.end(), [](const media::Media& x, const media::Media& y) {
        if (x.playable() != y.playable()) return x.playable();  // playable first
        int qx = qualityRank(x.videoResolution), qy = qualityRank(y.videoResolution);
        if (qx != qy) return qx > qy;                           // then best quality
        if (x.playable() && x.cached != y.cached) return x.cached;  // then cached debrid first
        return false;
    });
    return all;
}

struct SubtitleRequestHints {
    std::string videoHash;
    int64_t videoSize = 0;
    std::string filename;
};

/// Fan out /subtitles across the addons serving (type,id) and return the tracks
/// as neutral subtitle Streams (streamType 3, key = absolute SRT/VTT url).
/// Stremio's player forwards behaviorHints.videoHash/videoSize/filename from the
/// exact selected stream; do the same when the addon supplied them. We never
/// manufacture missing hashes with remote range reads. Deduplicate only identical
/// language/URL tracks; alternate releases in the same language remain available.
std::vector<media::Stream> resolveAllSubtitles(AddonEngine& engine, const std::string& stremioType,
    const std::string& stremioId, const SubtitleRequestHints& hints) {
    std::vector<media::Stream> out;
    std::set<std::string> seenTracks;
    std::vector<std::pair<std::string, std::string>> extra;
    if (!hints.videoHash.empty()) extra.emplace_back("videoHash", hints.videoHash);
    if (hints.videoSize > 0) extra.emplace_back("videoSize", std::to_string(hints.videoSize));
    if (!hints.filename.empty()) extra.emplace_back("filename", hints.filename);

    for (auto& a : engine.addonsFor("subtitles", stremioType, stremioId)) {
        std::string url = engine.resourceUrl(a, "subtitles", stremioType, stremioId, extra);
        std::vector<SubtitleOption> subs;
        try {
            subs = parseSubtitles(getSync(url, subtitleRequestTimeout()));
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio subtitles {}: {}", redactUrlForLog(url), ex.what());
            continue;
        }
        for (auto& s : subs) {
            std::string code = media::subtitleLangCode(s.lang);
            // dedup key: canonical code when known, else the raw lang verbatim
            std::string key = subtitleIdentity(code.empty() ? s.lang : code, s.id, s.url);
            if (s.url.empty() || !seenTracks.insert(key).second) continue;
            media::Stream st = subtitleOptionToStream(s);
            st.languageTag = code;  // already normalized above; keep dedupe/matching aligned
            out.push_back(std::move(st));
        }
    }
    return out;
}

}  // namespace

void flushPlaybackHistory() { playbackHistory().flush(); }

void beginPlayback(const std::string& id, const std::string& sessionId) {
    std::lock_guard<std::mutex> lock(completionMutex);
    const std::string key = playbackScope() + "\n" + id;
    completion.begin(key, sessionId);
}

bool playbackCompleted(const std::string& id) {
    std::lock_guard<std::mutex> lock(completionMutex);
    return !completion.canSave(playbackScope() + "\n" + id);
}

nlohmann::json savedPlayback(const std::string& id) {
    const std::string scope = playbackScope();
    return scope.empty() ? nlohmann::json::object() : playbackHistory().cachedLoad(scope, id);
}

void rememberPlayback(const media::Item& item, const media::Media& source, int64_t position, int64_t duration) {
    const std::string scope = playbackScope();
    if (scope.empty()) return;
    std::lock_guard<std::mutex> lock(completionMutex);
    if (!completion.canSave(scope + "\n" + item.ratingKey)) return;
    playbackHistory().save(scope, item, source, position, duration);
}

int savedPlaybackSource(const media::Item& item) {
    return PlaybackHistory::chooseSource(savedPlayback(item.ratingKey), item.media);
}

StremioBackend::StremioBackend() : engine(std::make_shared<AddonEngine>()) {
    // Browsable catalogs + composed home rows + ratings, always on. The account-
    // backed features (library = watchlist, watched flag, progress sync, continue
    // watching) are gated on a connected account (authKey persisted as the server
    // access token); without one, the backend is navigation/playback only.
    bool account = !AppConfig::instance().getToken().empty();
    caps_.sections = true;
    caps_.homeHubs = true;
    caps_.continueWatching = account;
    caps_.serverSort = false;
    caps_.serverFilter = false;
    caps_.genres = true;  // genre directories derived from the catalog's genre extra
    caps_.collections = false;
    caps_.playlists = false;
    caps_.related = false;
    caps_.personPages = false;
    caps_.globalSearch = false;
    caps_.recentlyAdded = false;
    caps_.markWatched = account;
    caps_.listKind = account ? media::ListKind::Favorites : media::ListKind::None;
    caps_.ratings = true;
    caps_.skipIntro = false;
    caps_.transcode = false;
    caps_.serverProgress = account;
    caps_.downloadOriginal = false;
    caps_.multiProfile = false;
}

// ---- navigation ----------------------------------------------------------------

void StremioBackend::listSections(media::Then<media::Container<media::Section>> then, media::OnError error) {
    // ONE sidebar section per content type (Films, Séries), plus an Anime
    // category that aggregates catalogs explicitly identified as anime.
    L10n loc = loadL10n();
    brls::async([engine = this->engine, loc, then, error]() {
        try {
            engine->ensureLoaded();
            media::Container<media::Section> c;
            for (auto& stype : engine->browsableTypes()) {
                if (stype != "movie" && stype != "series") continue;  // channel/tv: niche, skipped
                media::Section s;
                s.key = stype;            // bare type key, resolved to a catalog on demand
                s.type = mapType(stype);  // movie -> media movie, series -> media show
                s.title = typeLabel(loc, stype);
                c.Items.push_back(std::move(s));
            }
            media::Section anime;
            anime.key = "anime";
            // The UI represents Anime with its show view. Each catalog route
            // retains its original Stremio type for catalog requests, and meta
            // item identities continue to come from each returned item.
            anime.type = media::mediaTypeShow;
            anime.title = loc.anime;
            c.Items.push_back(std::move(anime));
            c.TotalRecordCount = (long)c.Items.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

std::vector<std::pair<std::string, std::string>> StremioBackend::sectionTabs(const std::string& sectionId) {
    // One sub-tab per browsable catalog of the section's type (Populaires,
    // Nouveautés, À la une, Public Domain…). Synchronous read of the already-
    // loaded engine; labels localized on the UI thread.
    L10n loc = loadL10n();
    std::vector<std::pair<std::string, std::string>> out;
    if (sectionId == "anime") {
        for (const auto& entry : animeCatalogs(*engine))
            out.emplace_back(catalogKey(entry.addon.base, entry.catalog.type, entry.catalog.id, entry.genreFilter),
                bestCatalogLabel(loc, entry.addon, entry.catalog));
        return out;
    }
    for (auto& pc : engine->catalogsForType(sectionId))
        out.emplace_back(
            catalogKey(pc.first.base, pc.second.type, pc.second.id), bestCatalogLabel(loc, pc.first, pc.second));
    return out;
}

void StremioBackend::getHomeHubs(
    int count, bool, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    int cnt = std::max(0, count);
    L10n loc = loadL10n();
    brls::async([engine = this->engine, cnt, loc, then, error]() {
        try {
            engine->ensureLoaded();
            // Interleave movie, series and anime catalogs before the Home row cap.
            std::vector<std::vector<AnimeCatalog>> groups(3);
            for (auto& p : engine->catalogsForType("movie")) groups[0].push_back({p.first, p.second, ""});
            for (auto& p : engine->catalogsForType("series")) groups[1].push_back({p.first, p.second, ""});
            groups[2] = animeCatalogs(*engine);
            auto cats = interleaveCatalogs(groups, [](const AnimeCatalog& entry) {
                return catalogKey(entry.addon.base, entry.catalog.type, entry.catalog.id, entry.genreFilter);
            });

            media::Container<media::Hub> out;
            const size_t maxHubs = 8;
            size_t requests = 0;
            for (auto& pc : cats) {
                if (out.Items.size() >= maxHubs || requests >= 12) break;
                ++requests;
                const Catalog& cat = pc.catalog;
                std::string url = animeCatalogUrl(pc);
                CatalogResult res;
                try {
                    res = parseCatalog(getSync(url));
                } catch (const std::exception& ex) {
                    brls::Logger::warning("stremio home catalog {}: {}", redactUrlForLog(url), ex.what());
                    continue;
                }
                if (res.items.empty()) continue;
                media::Hub h;
                h.title = (animeCatalogResults(classifyAnimeCatalog(cat.type, cat.id, cat.name, cat.genres, cat.hasGenre()),
                        pc.genreFilter) ? loc.anime : typeLabel(loc, cat.type)) +
                    " · " + bestCatalogLabel(loc, pc.addon, cat);
                h.hubIdentifier = "home.catalog." + std::to_string(out.Items.size());
                h.key = catalogKey(pc.addon.base, cat.type, cat.id, pc.genreFilter);  // "see all" -> getHubPage
                h.more = true;
                if ((int)res.items.size() > cnt) res.items.resize(cnt);
                h.items = std::move(res.items);
                out.Items.push_back(std::move(h));
            }
            out.TotalRecordCount = (long)out.Items.size();
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getSectionHubs(
    const std::string& sectionId, int count, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    // The "Suggestions" sub-tab: one row per browsable catalog, each
    // expandable to its full grid. Anime routes keep their optional genre filter.
    std::string stype = sectionId;  // "movie" | "series" | synthetic "anime"
    int cnt = std::max(0, count);
    L10n loc = loadL10n();
    brls::async([engine = this->engine, stype, cnt, loc, then, error]() {
        try {
            engine->ensureLoaded();
            media::Container<media::Hub> out;
            if (stype == "anime") {
                for (const auto& entry : animeCatalogs(*engine)) {
                    std::string url = animeCatalogUrl(entry);
                    CatalogResult res;
                    try {
                        res = parseCatalog(getSync(url));
                    } catch (const std::exception& ex) {
                        brls::Logger::warning("stremio anime section hub {}: {}", redactUrlForLog(url), ex.what());
                        continue;
                    }
                    if (res.items.empty()) continue;
                    media::Hub h;
                    h.title = bestCatalogLabel(loc, entry.addon, entry.catalog);
                    h.hubIdentifier = "section.anime." + std::to_string(out.Items.size());
                    h.key = catalogKey(
                        entry.addon.base, entry.catalog.type, entry.catalog.id, entry.genreFilter);
                    h.more = true;
                    if (static_cast<int>(res.items.size()) > cnt) res.items.resize(cnt);
                    h.items = std::move(res.items);
                    out.Items.push_back(std::move(h));
                }
                out.TotalRecordCount = static_cast<long>(out.Items.size());
                brls::sync(std::bind(then, std::move(out)));
                return;
            }
            for (auto& pc : engine->catalogsForType(stype)) {
                const Catalog& cat = pc.second;
                std::string url = buildCatalogUrl(pc.first.base, cat.type, cat.id);
                CatalogResult res;
                try {
                    res = parseCatalog(getSync(url));
                } catch (const std::exception& ex) {
                    brls::Logger::warning("stremio section hub {}: {}", redactUrlForLog(url), ex.what());
                    continue;
                }
                if (res.items.empty()) continue;
                media::Hub h;
                h.title = bestCatalogLabel(loc, pc.first, cat);
                h.hubIdentifier = "section.catalog." + cat.id;
                h.key = catalogKey(pc.first.base, cat.type, cat.id);
                h.more = true;
                if ((int)res.items.size() > cnt) res.items.resize(cnt);
                h.items = std::move(res.items);
                out.Items.push_back(std::move(h));
            }
            out.TotalRecordCount = (long)out.Items.size();
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getTopRated(media::MediaKind kind, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    if (kind != media::MediaKind::Movie && kind != media::MediaKind::Show) {
        if (error) error("IMDb Top is available only for movies and series");
        return;
    }
    const bool series = kind == media::MediaKind::Show;
    const size_t startCopy = start, sizeCopy = size;
    brls::async([kind, series, startCopy, sizeCopy, then, error]() {
        try {
            const int64_t now = (int64_t)std::time(nullptr);
            std::vector<media::Item> cached;
            int64_t fetchedAt = 0;
            bool fresh = false;
            {
                std::lock_guard<std::mutex> lock(imdbTopCache.mutex);
                auto& loaded = series ? imdbTopCache.seriesLoaded : imdbTopCache.moviesLoaded;
                auto& items = series ? imdbTopCache.series : imdbTopCache.movies;
                auto& stamp = series ? imdbTopCache.seriesFetchedAt : imdbTopCache.moviesFetchedAt;
                if (!loaded) {
                    loadImdbTopCacheFile(series, items, stamp);
                    loaded = true;
                }
                cached = items;
                fetchedAt = stamp;
                fresh = !cached.empty() && fetchedAt > 0 &&
                        (now <= fetchedAt || now - fetchedAt < IMDB_TOP_CACHE_TTL);
            }

            std::vector<media::Item> items = cached;
            if (!fresh) {
                try {
                    items = fetchImdbTop(kind);
                    int64_t refreshedAt = now > 0 ? now : 1;
                    {
                        std::lock_guard<std::mutex> lock(imdbTopCache.mutex);
                        auto& target = series ? imdbTopCache.series : imdbTopCache.movies;
                        auto& stamp = series ? imdbTopCache.seriesFetchedAt : imdbTopCache.moviesFetchedAt;
                        target = items;
                        stamp = refreshedAt;
                    }
                    saveImdbTopCacheFile(series, items, refreshedAt);
                } catch (const std::exception& ex) {
                    if (items.empty()) throw;
                    // Do not hammer IMDb again on every local pagination request.
                    // Mark only the in-memory snapshot as fresh for this session;
                    // the old on-disk timestamp is intentionally preserved so the
                    // next app launch will retry the refresh.
                    {
                        std::lock_guard<std::mutex> lock(imdbTopCache.mutex);
                        auto& stamp = series ? imdbTopCache.seriesFetchedAt : imdbTopCache.moviesFetchedAt;
                        stamp = now > 0 ? now : 1;
                    }
                    brls::Logger::warning("IMDb Top refresh failed; using cached snapshot: {}", ex.what());
                }
            }

            media::Container<media::Item> page = pageImdbTop(items, startCopy, sizeCopy);
            brls::sync(std::bind(then, std::move(page)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getContinueWatching(
    int count, media::Then<media::Container<media::Hub>> then, media::OnError error) {
    std::string key = accountKey();
    if (key.empty()) {
        emptyContainer<media::Hub>(then);
        return;
    }
    int cnt = std::max(0, count);
    std::string title = "main/home/resume"_i18n;  // resolved on the UI thread
    brls::async([engine = this->engine, key, cnt, title, then, error]() {
        try {
            nlohmann::json items = nlohmann::json::array();
            try {
                items = stremio::datastoreGet(key);
                if (!items.is_array()) items = nlohmann::json::array();
            } catch (const std::exception& ex) {
                brls::Logger::warning("stremio continue watching datastore: {}", ex.what());
            }
            if (accountKey() != key) return;
            engine->ensureLoaded();
            cacheEpisodeProgresses(key, items);
            auto localProgress = loadLocalProgresses();
            const std::string historyScope = playbackScope();
            const nlohmann::json historyRecords = historyScope.empty()
                ? nlohmann::json::object()
                : playbackHistory().records(historyScope);
            appendHistoryOnlyLibraryRows(items, historyRecords, localProgress);

            // Overlay the frequent local checkpoint on top of Stremio's coarser
            // account state. Remote sync happens immediately on the first 10 s
            // tick and then at most once per minute; after a crash/PS-button exit
            // this keeps Continue Watching at the last local checkpoint.
            std::vector<std::pair<std::string, nlohmann::json>> prog;
            for (auto& it : items) {
                nlohmann::json candidate = it;
                const std::string type = jstr(candidate, "type");
                const std::string baseId = jstr(candidate, "_id");
                const bool series = type == "series" || type == "anime";
                const EpisodeProgress* local = newestLocalProgress(localProgress, baseId, series);
                const auto remoteState = candidate.find("state");
                const std::string remoteTimestamp = remoteState != candidate.end() && remoteState->is_object()
                    ? jstr(*remoteState, "lastWatched") : std::string();
                if (local && local->timeOffset == 0 && localAtLeastAsNew(*local, remoteTimestamp)) {
                    if (!candidate.contains("state") || !candidate["state"].is_object())
                        candidate["state"] = nlohmann::json::object();
                    auto& state = candidate["state"];
                    state["timeOffset"] = 0;  // explicit local clear suppresses stale remote resume
                    if (local->duration > 0) state["duration"] = local->duration;
                    if (series) state["videoId"] = local->videoId;
                    if (!local->updatedIso.empty()) state["lastWatched"] = local->updatedIso;
                } else if (local) {
                    auto state = candidate.find("state");
                    const std::string remoteTimestamp = state != candidate.end() && state->is_object()
                        ? jstr(*state, "lastWatched")
                        : std::string();
                    if (localAtLeastAsNew(*local, remoteTimestamp)) {
                        if (state == candidate.end() || !state->is_object())
                            candidate["state"] = nlohmann::json::object();
                        auto& localState = candidate["state"];
                        localState["timeOffset"] = local->timeOffset;
                        if (local->duration > 0) localState["duration"] = local->duration;
                        if (series) localState["videoId"] = local->videoId;
                        // A local checkpoint represents an active rewatch even if
                        // the remote library previously marked the item watched.
                        localState["flaggedWatched"] = 0;
                        const std::string localTimestamp = !local->updatedIso.empty()
                            ? local->updatedIso
                            : PlaybackHistory::timestampIso(local->updated * 1000);
                        if (!localTimestamp.empty()) localState["lastWatched"] = localTimestamp;
                    }
                }

                auto st = candidate.find("state");
                if (st == candidate.end() || !st->is_object()) continue;
                if ((!series && jint(*st, "timeOffset") <= 0) || jint(*st, "flaggedWatched") > 0) continue;
                if (series && jstr(*st, "videoId").empty()) continue;
                std::string sortKey = jstr(*st, "lastWatched");
                prog.emplace_back(std::move(sortKey), std::move(candidate));
            }
            std::sort(prog.begin(), prog.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

            media::Container<media::Hub> out;
            media::Hub h;
            h.title = title;
            h.hubIdentifier = "home.continue";
            auto watched = loadWatchedEpisodes();
            for (size_t i = 0; i < prog.size() && static_cast<int>(h.items.size()) < cnt; i++) {
                const auto& libraryItem = prog[i].second;
                media::Item item = itemFromLibrary(libraryItem);
                auto state = libraryItem.find("state");
                std::string videoId = state == libraryItem.end() ? "" : jstr(*state, "videoId");
                std::string episodeKey = episodeId(item.guid, videoId, parseId(item.ratingKey).stremioType);
                if (item.type == media::mediaTypeShow) {
                    // An active checkpoint already identifies the episode to
                    // resume. Do not depend on an addon metadata request or an
                    // older partial episode to keep this series on Home.
                    if (resumeCheckpointEpisode(item, episodeKey)) {
                        h.items.push_back(std::move(item));
                        continue;
                    }
                    media::Item selected;
                    bool resolvedEpisodes = false;
                    const auto resourceType = parseId(item.ratingKey).stremioType;
                    for (const auto& addon : engine->addonsFor("meta", resourceType, item.guid)) {
                        try {
                            auto result = getSync(engine->resourceUrl(addon, "meta", resourceType, item.guid));
                            auto meta = result.find("meta");
                            if (meta == result.end() || !meta->is_object()) continue;
                            auto episodes = parseEpisodes(*meta, parseMeta(*meta));
                            // Some meta providers return only a series preview.
                            // Give the next provider a chance to resolve episodes.
                            if (episodes.empty()) continue;
                            resolvedEpisodes = true;
                            auto remote = loadEpisodeProgress(item.guid);
                            for (auto& episode : episodes) {
                                applyEpisodeWatched(episode, watched);
                                applyEpisodeProgress(episode, remote, localProgress, watched);
                            }
                            const bool completed = watched.count(episodeKey) ||
                                completedPosition(item.viewOffset, item.duration);
                            const bool partial = std::any_of(episodes.begin(), episodes.end(), episodePartial);
                            const int next = episodeContinuationIndex(episodes, completed && !partial ? episodeKey : "");
                            if (next >= 0) selected = episodes[next];
                            break;
                        } catch (const std::exception& ex) {
                            brls::Logger::warning("stremio continuation: {}", ex.what());
                        }
                    }
                    if (!applySeriesContinuation(item, selected, resolvedEpisodes)) continue;
                } else if (item.duration > 0 && item.viewOffset >= item.duration) {
                    continue;
                }
                h.items.push_back(std::move(item));
            }
            if (!h.items.empty()) out.Items.push_back(std::move(h));
            out.TotalRecordCount = (long)out.Items.size();
            brls::sync([key, then, out = std::move(out)]() mutable {
                if (accountKey() != key) return;
                then(std::move(out));
            });
        } catch (const std::exception& ex) {
            if (error && accountKey() == key) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getLibraryGrid(const std::string& sectionId, const media::GridQuery& q, size_t start,
    size_t size, media::Then<media::Container<media::Item>> then, media::OnError error) {
    // sectionId is a bare type ("movie"/"series") -> that type's primary
    // catalog, bare "anime" -> its first anime catalog, or a routed key
    // "base\ttype\tcatId[\tgenre]" from a hub or genre-filtered catalog.
    std::string sid = sectionId, genreId = q.genreId;
    size_t startCopy = start;
    brls::async([engine = this->engine, sid, genreId, startCopy, then, error]() {
        try {
            engine->ensureLoaded();
            std::string base, ctype, catId, routeGenre;
            CatalogRoute genreRoute;
            if (parseCatalogRouteKey(genreId, genreRoute)) {
                base = genreRoute.base; ctype = genreRoute.type; catId = genreRoute.id;
                routeGenre = genreRoute.genre;
            }
            bool supportsSkip = true;
            if (!base.empty() || isCatalogKey(sid)) {
                if (base.empty() && !splitCatalogKey(sid, base, ctype, catId, &routeGenre))
                    throw std::runtime_error("stremio: malformed section id");

                // Routed keys originate from a manifest catalog. Recover its
                // pagination capability from the already-loaded metadata; if a
                // stale key cannot be found, preserve the old behavior.
                for (auto& pc : engine->catalogsForType(ctype)) {
                    if (pc.first.base == base && pc.second.id == catId) {
                        supportsSkip = pc.second.hasSkip();
                        break;
                    }
                }
            } else if (sid == "anime") {
                auto cats = animeCatalogs(*engine);
                if (cats.empty()) throw std::runtime_error("stremio: no anime catalog");
                const auto& entry = cats.front();
                base = entry.addon.base;
                ctype = entry.catalog.type;
                catId = entry.catalog.id;
                routeGenre = entry.genreFilter;
                for (const auto& pc : engine->catalogsForType(ctype)) {
                    if (pc.first.base == base && pc.second.id == catId) {
                        supportsSkip = pc.second.hasSkip();
                        break;
                    }
                }
            } else {
                auto cats = engine->catalogsForType(sid);
                if (cats.empty()) throw std::runtime_error("stremio: no catalog for type");

                // Genre directories are built from the first catalog that actually
                // declares genre options. Keep the drill-down on that same catalog:
                // the first catalog for a type may belong to another addon and may
                // not support the selected genre at all, which produces an empty
                // grid even though the Genres page itself is populated.
                auto selected = cats.begin();
                if (!genreId.empty()) {
                    for (auto it = cats.begin(); it != cats.end(); ++it) {
                        const auto& genres = it->second.genres;
                        if (std::find(genres.begin(), genres.end(), genreId) != genres.end()) {
                            selected = it;
                            break;
                        }
                    }
                }
                base = selected->first.base;
                ctype = selected->second.type;
                catId = selected->second.id;
                supportsSkip = selected->second.hasSkip();
            }

            media::Container<media::Item> c;
            c.StartIndex = (long)startCopy;
            if (startCopy > 0 && !supportsSkip) {
                // The addon cannot address another page. Return an empty page
                // without repeating page zero or issuing a useless network GET.
                c.TotalRecordCount = (long)startCopy;
                brls::sync(std::bind(then, std::move(c)));
                return;
            }

            std::vector<std::pair<std::string, std::string>> extra;
            if (startCopy > 0) extra.emplace_back("skip", std::to_string(startCopy));
            if (!routeGenre.empty())
                extra.emplace_back("genre", routeGenre);
            else if (!genreId.empty())
                extra.emplace_back("genre", genreId);
            CatalogResult res = parseCatalog(getSync(buildCatalogUrl(base, ctype, catId, extra)));
            c.Items = std::move(res.items);
            // Stremio gives no total; report a running count (UI paginates via skip).
            c.TotalRecordCount = (long)(startCopy + c.Items.size());
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getCollectionChildren(
    const std::string&, size_t, size_t, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

void StremioBackend::getHubPage(
    const std::string& hubKey, size_t start, size_t size, media::Then<media::Container<media::Item>> then,
    media::OnError error) {
    // hubKey = "base\ttype\tcatId[\tgenre]" (set on home/section hubs).
    // Page via skip while preserving an optional catalog genre filter.
    std::string key = hubKey;
    size_t startCopy = start;
    brls::async([engine = this->engine, key, startCopy, then, error]() {
        try {
            engine->ensureLoaded();
            std::string base, ctype, catId, genre;
            if (!splitCatalogKey(key, base, ctype, catId, &genre))
                throw std::runtime_error("stremio: bad hub key");

            bool supportsSkip = true;
            for (auto& pc : engine->catalogsForType(ctype)) {
                if (pc.first.base == base && pc.second.id == catId) {
                    supportsSkip = pc.second.hasSkip();
                    break;
                }
            }

            media::Container<media::Item> c;
            c.StartIndex = (long)startCopy;
            if (startCopy > 0 && !supportsSkip) {
                c.TotalRecordCount = (long)startCopy;
                brls::sync(std::bind(then, std::move(c)));
                return;
            }

            std::vector<std::pair<std::string, std::string>> extra;
            if (startCopy > 0) extra.emplace_back("skip", std::to_string(startCopy));
            if (!genre.empty()) extra.emplace_back("genre", genre);
            CatalogResult res = parseCatalog(getSync(buildCatalogUrl(base, ctype, catId, extra)));
            c.Items = std::move(res.items);
            c.TotalRecordCount = (long)(startCopy + c.Items.size());
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getItemDetail(
    const std::string& id, bool full, media::Then<media::Item> then, media::OnError error) {
    getDetail(id, full, false, std::move(then), std::move(error));
}

void StremioBackend::completePlaybackSources(media::Item item,
    media::Then<media::Item> then, media::OnError error) {
    brls::async([engine = this->engine, item = std::move(item), then, error]() mutable {
        try {
            engine->ensureLoaded();
            const auto id = parseId(item.ratingKey);
            item.media = resolveAllStreams(*engine, id.stremioType, id.stremioId, "", nullptr, std::move(item.media));
            item.sourcesComplete = true;
            brls::sync([then, item = std::move(item)]() mutable { then(std::move(item)); });
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getResumeDetail(const std::string& id, bool reuseSource,
    media::Then<media::Item> then, media::OnError error) {
    getDetail(id, true, reuseSource, std::move(then), std::move(error));
}

void StremioBackend::getDetail(
    const std::string& id, bool full, bool reuseSource, media::Then<media::Item> then, media::OnError error) {
    ParsedId pid = parseId(id);
    // Meta is only addressable on a movie or a whole series; episodes/seasons
    // resolve through the parent series meta (the object carrying videos[]).
    bool isEpisode = ((pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0);
    bool playable = (pid.stremioType == "movie") || isEpisode;
    std::string metaType = pid.stremioType == "anime" ? "anime" : (pid.stremioType == "movie" ? "movie" : "series");
    std::string metaId = pid.baseId;  // movie/show id without any season:episode suffix
    std::string ratingKey = id;

    brls::async([engine = this->engine, ratingKey, full, reuseSource, isEpisode, playable, metaType, metaId, then, error]() {
        try {
            engine->ensureLoaded();
            auto addons = engine->addonsFor("meta", metaType, metaId);
            if (addons.empty()) throw std::runtime_error("stremio: no meta addon for item");

            // First addon that returns a meta object wins.
            nlohmann::json metaObj;
            bool found = false;
            for (auto& a : addons) {
                std::string url = engine->resourceUrl(a, "meta", metaType, metaId);
                nlohmann::json j;
                try {
                    j = getSync(url);
                } catch (const std::exception& ex) {
                    brls::Logger::warning("stremio meta {}: {}", redactUrlForLog(url), ex.what());
                    continue;
                }
                auto meta = j.find("meta");
                if (meta != j.end() && meta->is_object()) {
                    metaObj = *meta;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("stremio: meta not found");

            media::Item out;
            if (isEpisode) {
                // Pick the matching episode video out of the series meta.
                media::Item show = parseMeta(metaObj);
                for (auto& e : parseEpisodes(metaObj, show))
                    if (e.ratingKey == ratingKey) {
                        out = std::move(e);
                        break;
                    }
                if (out.ratingKey.empty()) {  // not found: minimal episode stub
                    out.ratingKey = ratingKey;
                    out.key = ratingKey;
                    out.type = media::mediaTypeEpisode;
                }
                applyEpisodeWatched(out, loadWatchedEpisodes());
            } else {
                out = parseMeta(metaObj);
            }

            // Detail and Choose another source resolve all providers. Resume first
            // refreshes the saved provider/release, and requests the remaining
            // providers only when that release is unavailable. Movie and episode
            // sources retain non-playable rows so the picker can explain them.
            if (full && playable) {
                ParsedId sp = parseId(ratingKey);
                std::string streamType = sp.stremioType;
                std::string savedIdentity;
                if (reuseSource) {
                    const auto scope = playbackScope();
                    auto record = scope.empty() ? nlohmann::json::object() : playbackHistory().load(scope, ratingKey);
                    auto source = record.find("source");
                    if (source != record.end() && source->is_object()) savedIdentity = jstr(*source, "identity");
                }
                out.media = resolveAllStreams(*engine, streamType, sp.stremioId, savedIdentity, &out.sourcesComplete);
            }
            if (playable) {
                const ParsedId progressId = parseId(ratingKey);
                const EpisodeProgress remote = isEpisode ? loadEpisodeProgress(progressId.baseId) : EpisodeProgress{};
                applyEpisodeProgress(out, remote, loadLocalProgresses(), loadWatchedEpisodes());
            }
            brls::sync(std::bind(then, std::move(out)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getChildren(
    const std::string& id, media::Then<media::Container<media::Item>> then, media::OnError error) {
    ParsedId pid = parseId(id);
    std::string stremioType = pid.stremioType;
    std::string stremioId = pid.stremioId;

    // movie: no children.
    if (stremioType == "movie") {
        emptyContainer<media::Item>(then);
        return;
    }

    std::string ratingKeyCopy = id;
    brls::async([engine = this->engine, ratingKeyCopy, stremioType, stremioId, then, error]() {
        try {
            engine->ensureLoaded();
            // Both "series" (show -> seasons) and "season" (-> episodes) resolve
            // through the series meta (the only object carrying videos[]).
            ParsedId pid = parseId(ratingKeyCopy);
            std::string showId = pid.season >= 0 ? pid.baseId : stremioId;

            std::string metaType = pid.stremioType == "anime" ? "anime" : "series";
            auto addons = engine->addonsFor("meta", metaType, showId);
            if (addons.empty()) throw std::runtime_error("stremio: no meta addon for series");

            nlohmann::json metaObj;
            bool found = false;
            for (auto& a : addons) {
                std::string url = engine->resourceUrl(a, "meta", metaType, showId);
                nlohmann::json j;
                try {
                    j = getSync(url);
                } catch (const std::exception& ex) {
                    brls::Logger::warning("stremio meta {}: {}", redactUrlForLog(url), ex.what());
                    continue;
                }
                auto meta = j.find("meta");
                if (meta != j.end() && meta->is_object()) {
                    metaObj = *meta;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("stremio: series meta not found");

            // Build the show stub used to back-reference episodes (grandparent*).
            media::Item show = parseMeta(metaObj);
            // parseMeta sets type=show + ratingKey="series:{id}" from meta.id.

            media::Container<media::Item> c;
            if ((stremioType == "series" || stremioType == "anime") && pid.season < 0) {
                // Show -> synthesize one Season per distinct video.season.
                std::map<int64_t, int64_t> episodeCounts;  // season -> #episodes
                auto vids = metaObj.find("videos");
                if (vids != metaObj.end() && vids->is_array())
                    for (auto& v : *vids) episodeCounts[jint(v, "season")]++;

                for (auto& kv : episodeCounts) {  // std::map iterates ascending
                    int64_t n = kv.first;
                    media::Item s;
                    s.ratingKey = seasonId(show.guid, n, stremio::stremioType(show));  // "season:{showId}:{n}"
                    s.key = s.ratingKey;
                    s.type = media::mediaTypeSeason;
                    s.index = n;
                    s.title = (n == 0) ? "Specials" : ("Season " + std::to_string(n));
                    s.leafCount = kv.second;
                    s.thumb = show.thumb;  // seasons reuse the show poster
                    s.art = show.art;
                    s.grandparentRatingKey = show.ratingKey;
                    s.grandparentTitle = show.title;
                    s.parentTitle = show.title;
                    c.Items.push_back(std::move(s));
                }
            } else {  // "season" -> episodes of that season
                int64_t wantSeason = pid.season;
                auto watched = loadWatchedEpisodes();
                auto progress = loadEpisodeProgress(showId);
                auto localProgress = loadLocalProgresses();
                auto all = parseEpisodes(metaObj, show);
                for (auto& e : all) {
                    if (e.parentIndex != wantSeason) continue;
                    applyEpisodeWatched(e, watched);
                    applyEpisodeProgress(e, progress, localProgress, watched);
                    c.Items.push_back(std::move(e));
                }
            }
            c.TotalRecordCount = (long)c.Items.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getAllEpisodes(const std::string& showId, bool,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    ParsedId pid = parseId(showId);
    std::string baseId = (pid.episode >= 0 || pid.season >= 0) ? pid.baseId : pid.stremioId;
    std::string idCopy = showId;
    std::string metaType = pid.stremioType == "anime" ? "anime" : "series";
    brls::async([engine = this->engine, idCopy, baseId, metaType, then, error]() {
        try {
            engine->ensureLoaded();
            auto addons = engine->addonsFor("meta", metaType, baseId);
            if (addons.empty()) throw std::runtime_error("stremio: no meta addon for series");

            nlohmann::json metaObj;
            bool found = false;
            for (auto& a : addons) {
                std::string url = engine->resourceUrl(a, "meta", metaType, baseId);
                nlohmann::json j;
                try {
                    j = getSync(url);
                } catch (const std::exception& ex) {
                    brls::Logger::warning("stremio meta {}: {}", redactUrlForLog(url), ex.what());
                    continue;
                }
                auto meta = j.find("meta");
                if (meta != j.end() && meta->is_object()) {
                    metaObj = *meta;
                    found = true;
                    break;
                }
            }
            if (!found) throw std::runtime_error("stremio: series meta not found");

            media::Item show = parseMeta(metaObj);
            media::Container<media::Item> c;
            c.Items = parseEpisodes(metaObj, show);  // already sorted (season, episode)
            auto watched = loadWatchedEpisodes();
            auto progress = loadEpisodeProgress(baseId);
            auto localProgress = loadLocalProgresses();
            if (const auto* latest = newestLocalProgress(localProgress, baseId, true)) {
                if (localAtLeastAsNew(*latest, progress.updatedIso)) {
                    progress = *latest;
                    setEpisodeProgress(accountKey(), baseId, progress);
                }
            }
            for (auto& e : c.Items) {
                applyEpisodeWatched(e, watched);
                applyEpisodeProgress(e, progress, localProgress, watched);
            }
            c.TotalRecordCount = (long)c.Items.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getNextUp(
    const std::string& showId, std::function<void(media::Item, bool)> then, media::OnError) {
    const auto show = parseId(showId);
    getAllEpisodes(showId, false, [then, show](media::Container<media::Item> episodes) {
        const auto progress = loadEpisodeProgress(show.baseId);
        std::string completedKey;
        const std::string lastKey = episodeId(show.baseId, progress.videoId, show.stremioType);
        for (const auto& episode : episodes.Items)
            if (episode.ratingKey == lastKey && episodeCompleted(episode)) completedKey = lastKey;
        const bool partial = std::any_of(episodes.Items.begin(), episodes.Items.end(), [](const media::Item& e) {
            return episodePartial(e);
        });
        const int index = episodeContinuationIndex(episodes.Items, partial ? "" : completedKey);
        then(index >= 0 ? episodes.Items[index] : media::Item{}, false);
    }, [then](const std::string&) { then(media::Item{}, false); });
}

void StremioBackend::getExtras(
    const std::string&, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

void StremioBackend::getRelated(
    const std::string&, int, media::Then<media::Container<media::Hub>> then, media::OnError) {
    emptyContainer<media::Hub>(then);
}

void StremioBackend::getPersonMedia(
    const std::string&, int, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

void StremioBackend::search(const std::string& query, media::MediaKind kind, int limit,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    std::string q = query;
    int lim = limit;
    std::set<std::string> wantTypes = kindToStremioTypes(kind);
    brls::async([engine = this->engine, q, lim, wantTypes, then, error]() {
        try {
            engine->ensureLoaded();
            auto originalCatalogs = engine->allCatalogs();
            std::vector<std::vector<std::pair<Addon, Catalog>>> groups(3);
            for (auto& pc : originalCatalogs) {
                if (!pc.second.hasSearch() || (!wantTypes.empty() && !wantTypes.count(pc.second.type))) continue;
                size_t group = pc.second.type == "movie" ? 0 : pc.second.type == "series" ? 1 : 2;
                groups[group].push_back(pc);
            }
            auto catalogs = interleaveCatalogs(groups, [](const std::pair<Addon, Catalog>& entry) {
                return catalogKey(entry.first.base, entry.second.type, entry.second.id);
            });
            media::Container<media::Item> c;
            std::set<std::string> seen;
            std::vector<std::vector<media::Item>> rows;
            size_t requests = 0;
            for (auto& pc : catalogs) {
                if (lim <= 0 || requests >= 12) break;
                const Addon& addon = pc.first;
                const Catalog& cat = pc.second;
                if (!cat.hasSearch()) continue;
                if (!wantTypes.empty() && wantTypes.count(cat.type) == 0) continue;

                std::string url = engine->resourceUrl(addon, "catalog", cat.type, cat.id, {{"search", q}});
                ++requests;
                CatalogResult res;
                try {
                    res = parseCatalog(getSync(url));
                } catch (const std::exception& ex) {
                    brls::Logger::warning("stremio search {}: {}", redactUrlForLog(url), ex.what());
                    continue;
                }
                rows.push_back(std::move(res.items));
            }
            for (size_t row = 0; static_cast<int>(c.Items.size()) < lim; ++row) {
                bool any = false;
                for (auto& items : rows) {
                    if (row >= items.size()) continue;
                    any = true;
                    auto& item = items[row];
                    if (!item.ratingKey.empty() && seen.insert(item.ratingKey).second)
                        c.Items.push_back(std::move(item));
                    if (static_cast<int>(c.Items.size()) >= lim) break;
                }
                if (!any) break;
            }
            c.TotalRecordCount = (long)c.Items.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getRecentlyAdded(
    size_t, size_t, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

void StremioBackend::getGenres(const std::string& sectionId, media::MediaKind,
    media::Then<media::Container<media::Section>> then, media::OnError error) {
    // Keep each genre attached to the exact catalog that advertised it.
    std::string stype = sectionId;  // "movie" | "series" | "anime"
    // English genre -> localized label, resolved on the UI thread. The Section
    // key retains the catalog route and raw genre value; the title is localized.
    std::map<std::string, std::string> gmap = {
        {"Action", "main/stremio/genre/action"_i18n}, {"Adventure", "main/stremio/genre/adventure"_i18n},
        {"Animation", "main/stremio/genre/animation"_i18n}, {"Biography", "main/stremio/genre/biography"_i18n},
        {"Comedy", "main/stremio/genre/comedy"_i18n}, {"Crime", "main/stremio/genre/crime"_i18n},
        {"Documentary", "main/stremio/genre/documentary"_i18n}, {"Drama", "main/stremio/genre/drama"_i18n},
        {"Family", "main/stremio/genre/family"_i18n}, {"Fantasy", "main/stremio/genre/fantasy"_i18n},
        {"History", "main/stremio/genre/history"_i18n}, {"Horror", "main/stremio/genre/horror"_i18n},
        {"Mystery", "main/stremio/genre/mystery"_i18n}, {"Romance", "main/stremio/genre/romance"_i18n},
        {"Sci-Fi", "main/stremio/genre/scifi"_i18n}, {"Sport", "main/stremio/genre/sport"_i18n},
        {"Thriller", "main/stremio/genre/thriller"_i18n}, {"War", "main/stremio/genre/war"_i18n},
        {"Western", "main/stremio/genre/western"_i18n}};
    brls::async([engine = this->engine, stype, gmap, then, error]() {
        try {
            engine->ensureLoaded();
            media::Container<media::Section> c;
            auto catalogs = engine->catalogsForType(stype);
            if (stype == "anime") {
                catalogs.clear();
                for (const auto& entry : animeCatalogs(*engine)) catalogs.emplace_back(entry.addon, entry.catalog);
            }
            std::set<std::string> seenGenres;
            for (auto& pc : catalogs) {
                if (!pc.second.hasGenre() || pc.second.genres.empty()) continue;
                for (auto& g : pc.second.genres) {
                    if (g.empty() || !seenGenres.insert(g).second) continue;
                    media::Section s;
                    s.key = catalogKey(pc.first.base, pc.second.type, pc.second.id, g);
                    auto it = gmap.find(g);
                    s.title = (it != gmap.end()) ? it->second : g;  // localized display
                    s.type = mapType(stype);
                    c.Items.push_back(std::move(s));
                }
            }
            c.TotalRecordCount = (long)c.Items.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getCollections(
    const std::string&, size_t, size_t, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

void StremioBackend::getPlaylists(
    size_t, size_t, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

void StremioBackend::getPlaylistItems(
    const std::string&, size_t, size_t, media::Then<media::Container<media::Item>> then, media::OnError) {
    emptyContainer<media::Item>(then);
}

// ---- item actions --------------------------------------------------------------

void StremioBackend::markWatched(const std::string& id) {
    std::string key = accountKey();
    if (key.empty()) return;
    std::string rk = id;
    ParsedId pid = parseId(rk);
    bool episode = (pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0;
    std::lock_guard<std::mutex> completionLock(completionMutex);
    if (!completion.accept(playbackScope() + "\n" + rk, true)) return;
    const auto generation = reserveRemoteProgressSync(key, rk, media::PlayState::Stopped, (int64_t)std::time(nullptr));
    queueLocalCompletion(rk, pid, 0, (int64_t)std::time(nullptr));

    brls::async([engine = this->engine, rk, key, pid, episode, generation]() {
        try {
            std::lock_guard<std::mutex> lock(libraryWriteMutex);
            if (!isLatestRemoteProgressSync(key, rk, generation)) return;
            upsertLibrary(*engine, key, rk, [episode, videoId = pid.stremioId](nlohmann::json& st) {
                // An episode must not mark the whole series watched. The local
                // episode history drives the checkmark/next-up state instead.
                st["flaggedWatched"] = episode ? 0 : 1;
                st["timeOffset"] = 0;  // watched -> clear resume position
                if (episode) st["videoId"] = videoId;
            });
            if (episode) setEpisodeProgress(key, pid.baseId, {pid.baseId, pid.stremioId, 0, 0, 0});
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio markWatched: {}", ex.what());
        }
    });
}

void StremioBackend::markUnwatched(const std::string& id) {
    std::string key = accountKey();
    if (key.empty()) return;
    std::string rk = id;
    ParsedId pid = parseId(rk);
    bool episode = (pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0;
    std::lock_guard<std::mutex> completionLock(completionMutex);
    completion.unwatch(playbackScope() + "\n" + rk);
    const auto generation = reserveRemoteProgressSync(key, rk, media::PlayState::Stopped, (int64_t)std::time(nullptr));
    if (episode) setEpisodeWatched(rk, false);

    brls::async([engine = this->engine, rk, key, generation]() {
        try {
            std::lock_guard<std::mutex> lock(libraryWriteMutex);
            if (!isLatestRemoteProgressSync(key, rk, generation)) return;
            upsertLibrary(*engine, key, rk, [](nlohmann::json& st) { st["flaggedWatched"] = 0; });
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio markUnwatched: {}", ex.what());
        }
    });
}

// ---- playback (étape 2) --------------------------------------------------------

media::PlaybackSource StremioBackend::resolvePlayback(
    const media::Item&, const media::Media& version, const media::PlaybackOptions& opts) {
    // getItemDetail already fanned out /stream and stored the chosen playback URL
    // in version.parts[0].key (Stremio has no per-request transcode decision). An
    // empty url means no playable source -> the player shows a "playback failed"
    // dialog. We never throw: a cross-TU throw on the borealis async task loop
    // (which does not wrap tasks in try/catch) would abort the app.
    if (version.parts.empty() || version.parts.front().key.empty()) return {};
    std::string extra = "network-timeout=" + std::to_string(HTTP::TIMEOUT / 100);
    if (opts.seekMs > 0) extra += ",start=" + misc::sec2Time(opts.seekMs / 1000);
    if (HTTP::PROXY_STATUS) extra += ",http-proxy=\"" + HTTP::PROXY + "\"";
    return {version.parts.front().key, extra, false, "directplay"};
}

std::string StremioBackend::subtitleSidecarUrl(const std::string& streamKey) const {
    // Stremio subtitle urls are absolute http(s) links (like posters) — passed to
    // mpv sub-add verbatim, no proxy/token (addons are unauthenticated).
    return streamKey;
}

void StremioBackend::getSubtitles(const media::Item& item, const media::Media& version,
    media::Then<std::vector<media::Stream>> then, media::OnError error) {
    ParsedId pid = parseId(item.ratingKey);
    // Subtitles are addressable only on a playable video (a whole movie, or an
    // episode — its id carries the season:episode suffix). Shows/seasons play via
    // their episodes, so they never reach here with a subtitle request.
    bool isEpisode = ((pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0);
    if (pid.stremioType != "movie" && !isEpisode) {
        if (then) then({});
        return;
    }

    SubtitleRequestHints hints;
    if (!version.parts.empty()) {
        const auto& part = version.parts.front();
        hints.videoHash = part.videoHash;
        hints.videoSize = part.size;
        hints.filename = part.filename;
    }

    std::string type = pid.stremioType;
    std::string id = pid.stremioId;  // movie tt-id, or episode "tt…:S:E"
    brls::async([engine = this->engine, type, id, hints, then, error]() {
        try {
            engine->ensureLoaded();
            std::vector<media::Stream> subs = resolveAllSubtitles(*engine, type, id, hints);
            brls::sync(std::bind(then, std::move(subs)));
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio getSubtitles: {}", ex.what());
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

std::string StremioBackend::subtitleMenuHint() const {
    // When the account's addon collection carries no `subtitles` provider, no
    // external subtitles can ever be fetched — guide the user to install one
    // (mirrors the streams "none" hint). hasResource reads the already-loaded
    // manifest set (loaded during playback); it never blocks here.
    if (engine->hasResource("subtitles")) return "";
    return "main/stremio/subtitle/none"_i18n;
}

void StremioBackend::reportProgress(
    const std::string& id, media::PlayState state, int64_t posMs, int64_t durMs, const std::string&) {
    std::string key = accountKey();
    if (posMs < 0) return;

    ParsedId pid = parseId(id);
    const bool episode = (pid.stremioType == "series" || pid.stremioType == "anime") && pid.episode >= 0;
    const bool watched = completedPosition(posMs, durMs);
    std::lock_guard<std::mutex> completionLock(completionMutex);
    const std::string completionKey = playbackScope() + "\n" + id;
    if (!completion.accept(completionKey, watched)) return;
    if (key.empty()) return;
    const int64_t now = (int64_t)std::time(nullptr);
    const std::string reportedAt = stremio::nowIso();
    const std::string videoId = episode ? pid.stremioId : "";

    // Queue legacy checkpoints on the existing history writer; player ticks
    // never open or rewrite local JSON files.
    // A completed item clears its local resume point.
    if (watched) queueLocalCompletion(id, pid, durMs, now);
    else storeLocalProgress(id, {pid.baseId, videoId, posMs, durMs, now});

    // Keep account traffic bounded: sync the first Playing checkpoint, then at
    // most once per minute, plus every pause/stop. A generation ticket prevents
    // an older queued tick from overwriting a newer final state.
    const int64_t generation = reserveRemoteProgressSync(key, id, watched ? media::PlayState::Stopped : state, now);
    if (generation == 0) return;

    std::string rk = id;
    int64_t pos = posMs, dur = durMs;
    brls::async([engine = this->engine, rk, key, pid, episode, videoId, pos, dur, watched, generation, now, reportedAt]() {
        try {
            std::lock_guard<std::mutex> lock(libraryWriteMutex);
            if (!isLatestRemoteProgressSync(key, rk, generation)) return;
            upsertLibrary(*engine, key, rk, [pos, dur, videoId, episode, watched](nlohmann::json& st) {
                // Local completed checkpoints are cleared, but account episode
                // progress retains the terminal offset. A second device can then
                // infer the same >=90% completion without GMCA's local watched set.
                st["timeOffset"] = remotePlaybackOffset(episode, watched, pos);
                if (dur > 0) st["duration"] = dur;
                if (episode) {
                    st["flaggedWatched"] = 0;
                    st["videoId"] = videoId;
                } else {
                    st["flaggedWatched"] = watched ? 1 : 0;
                }
            }, reportedAt);
            if (episode) {
                EpisodeProgress progress{pid.baseId, videoId, watched ? 0 : pos, dur, now};
                progress.updatedIso = reportedAt;
                setEpisodeProgress(key, pid.baseId, std::move(progress));
            }
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio reportProgress: {}", ex.what());
        }
    });
}

// ---- url helpers ---------------------------------------------------------------

std::string StremioBackend::imageUrl(const std::string& path, int, int) const {
    // Stremio posters/backdrops/logos are ABSOLUTE URLs — no proxy/resize.
    return path;
}

std::string StremioBackend::downloadUrl(const std::string& partKey) const { return partKey; }

HTTP::Header StremioBackend::authHeaders() const { return HTTP::Header{}; }

// ---- personal list: Stremio account library (= watchlist) ----------------------

bool StremioBackend::canList(const media::Item& item) const {
    if (caps_.listKind == media::ListKind::None) return false;
    // Only whole movies/series live in the library (not seasons/episodes).
    return item.type == media::mediaTypeMovie || item.type == media::mediaTypeShow;
}

void StremioBackend::listWatchlist(const std::string&, media::MediaKind kind, size_t start, size_t size,
    media::Then<media::Container<media::Item>> then, media::OnError error) {
    std::string key = accountKey();
    if (key.empty()) {
        emptyContainer<media::Item>(then);
        return;
    }
    std::set<std::string> wantTypes = kindToStremioTypes(kind);
    size_t s = start, n = size;
    brls::async([key, wantTypes, s, n, then, error]() {
        try {
            nlohmann::json items = stremio::datastoreGet(key);
            std::vector<media::Item> all;
            for (auto& it : items) {
                if (jbool(it, "removed")) continue;  // only items kept in the library
                std::string t = jstr(it, "type");
                if (!wantTypes.empty() && wantTypes.count(t) == 0) continue;
                all.push_back(itemFromLibrary(it));
            }
            media::Container<media::Item> c;
            for (size_t i = s; i < all.size() && i < s + n; i++) c.Items.push_back(std::move(all[i]));
            c.StartIndex = (long)s;
            c.TotalRecordCount = (long)all.size();
            brls::sync(std::bind(then, std::move(c)));
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::getWatchlistState(const media::Item& item, media::Then<bool> then, media::OnError error) {
    std::string key = accountKey();
    if (key.empty()) {
        if (then) then(false);
        return;
    }
    std::string baseId = parseId(item.ratingKey).baseId;
    brls::async([key, baseId, then, error]() {
        try {
            nlohmann::json items = stremio::datastoreGet(key);
            bool in = false;
            for (auto& it : items)
                if (jstr(it, "_id") == baseId && !jbool(it, "removed")) {
                    in = true;
                    break;
                }
            brls::sync([then, in]() {
                if (then) then(in);
            });
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

void StremioBackend::setWatchlisted(
    const media::Item& item, bool add, std::function<void()> then, media::OnError error) {
    std::string key = accountKey();
    if (key.empty()) {
        if (error) error("Compte Stremio requis");
        return;
    }
    ParsedId pid = parseId(item.ratingKey);
    std::string baseId = pid.baseId;
    std::string libType = pid.stremioType == "anime" ? "anime" : (pid.stremioType == "movie" ? "movie" : "series");
    std::string title = item.title, poster = item.thumb;
    int64_t dur = item.duration;
    bool addCopy = add;
    brls::async([key, baseId, libType, title, poster, dur, addCopy, then, error]() {
        try {
            // Serialize every read-modify-write of the Stremio library. Without
            // this, a concurrent progress/watched update can land between this
            // GET and PUT and be overwritten by a stale watchlist snapshot.
            std::lock_guard<std::mutex> lock(libraryWriteMutex);
            // Reuse an existing entry (preserve its playback state); else build one.
            nlohmann::json items = stremio::datastoreGet(key);
            nlohmann::json found;
            for (auto& it : items)
                if (jstr(it, "_id") == baseId) {
                    found = it;
                    break;
                }
            std::string now = stremio::nowIso();
            if (found.is_null()) {
                found = {
                    {"_id", baseId}, {"name", title}, {"type", libType}, {"poster", poster},
                    {"posterShape", "poster"}, {"removed", !addCopy}, {"temp", false}, {"_ctime", now},
                    {"_mtime", now},
                    {"state", {{"lastWatched", nullptr}, {"timeWatched", 0}, {"timeOffset", 0},
                                  {"overallTimeWatched", 0}, {"timesWatched", 0}, {"flaggedWatched", 0},
                                  {"duration", dur}, {"videoId", nullptr}, {"watched", nullptr},
                                  {"noNotif", false}}},
                    {"behaviorHints", nlohmann::json::object()},
                };
            } else {
                found["removed"] = !addCopy;
                found["temp"] = false;  // explicit library membership, not a transient watch
                if (!title.empty()) found["name"] = title;
                if (!poster.empty()) found["poster"] = poster;
                found["_mtime"] = now;
            }
            stremio::datastorePut(key, found);
            if (then) brls::sync(then);
        } catch (const std::exception& ex) {
            if (error) brls::sync(std::bind(error, std::string(ex.what())));
        }
    });
}

}  // namespace stremio
