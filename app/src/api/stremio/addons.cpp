/*
    GMCA — Stremio addon engine implementation (see stremio/addons.hpp).
    Re-syncs the account's addon collection, then loads the configured
    transportUrls' manifests once and routes resource queries across them.
*/

#include "api/stremio/addons.hpp"
#include "api/stremio/auth.hpp"
#include "api/stremio/requests.hpp"
#include "utils/config.hpp"
#include <borealis/core/logger.hpp>
#include <algorithm>
#include <map>

namespace stremio {

void AddonEngine::ensureLoaded() {
    std::lock_guard<std::mutex> lock(mtx);
    if (loaded) return;

    // Re-sync the account's addon collection before loading manifests. Stremio
    // (re)configures an addon by REPLACING its transportUrl (e.g. Torrentio's
    // debrid apikey lives in the URL path) and the official clients pull the
    // collection at every startup — a list snapshotted at login goes stale and
    // keeps serving raw-torrent streams instead of debrid links (GH #46). On
    // failure (offline, expired key) keep the stored list; an empty collection
    // is ignored too (a live account always has the default addons) rather
    // than wiping a working list.
    const std::string& authKey = AppConfig::instance().getToken();
    if (!authKey.empty()) {
        try {
            std::vector<std::string> fresh = fetchAddonCollection(authKey);
            if (!fresh.empty()) AppConfig::instance().setStremioAddons(fresh);
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio: addon collection sync failed: {}", ex.what());
        }
    }

    // AppConfig::instance().getStremioAddons() returns the configured list of
    // transportUrls (each ending in /manifest.json). Provided by the config layer.
    const std::vector<std::string>& transports = AppConfig::instance().getStremioAddons();

    // Manifest requests are independent. Register them as a lazy bounded batch
    // before consuming them in collection order below. The first getSync starts
    // the batch; parsing/error handling remains exactly as before.
    requests::registerBatch(transports);

    addons.clear();
    addons.reserve(transports.size());
    for (const auto& transport : transports) {
        try {
            nlohmann::json j = getSync(transport);
            if (j.empty()) {
                brls::Logger::warning("stremio: empty manifest from {}", transport);
                continue;
            }
            Addon a;
            a.transportUrl = transport;
            a.base = baseFromTransport(transport);
            a.manifest = parseManifest(j);
            addons.push_back(std::move(a));
        } catch (const std::exception& ex) {
            brls::Logger::warning("stremio: manifest load failed {}: {}", transport, ex.what());
        }
    }
    loaded = true;
}

void AddonEngine::invalidate() {
    std::lock_guard<std::mutex> lock(mtx);
    loaded = false;
    requests::clear();
}

std::vector<Addon> AddonEngine::addonsFor(
    const std::string& resource, const std::string& type, const std::string& id) {
    std::vector<Addon> out;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& a : addons)
            if (a.supports(resource, type, id)) out.push_back(a);
    }

    // Meta/stream/subtitle fallbacks are consumed serially by backend.cpp to
    // preserve addon precedence. Coalesce the underlying HTTP waits so a slow
    // provider no longer adds its entire timeout after every previous provider.
    if (out.size() > 1) {
        std::vector<std::string> urls;
        urls.reserve(out.size());
        for (const auto& a : out) urls.push_back(resourceUrl(a, resource, type, id));
        long timeout = (resource == "stream" || resource == "subtitles") ? 15000L : HTTP::TIMEOUT;
        requests::registerBatch(urls, timeout);
    }
    return out;
}

bool AddonEngine::hasResource(const std::string& resource) const {
    // Called from the UI thread (subtitle menu). ensureLoaded() holds mtx across
    // its network fetches, so a blocking lock here could freeze the UI for
    // seconds. Try the lock instead and treat contention (worker still loading)
    // like "not loaded yet" -> return true, so no misleading "install an addon"
    // hint is shown while we can't actually inspect the collection.
    std::unique_lock<std::mutex> lock(mtx, std::try_to_lock);
    if (!lock.owns_lock() || !loaded) return true;
    for (const auto& a : addons)
        if (a.manifest.resources.count(resource)) return true;
    return false;
}

std::vector<std::pair<Addon, Catalog>> AddonEngine::allCatalogs() {
    std::vector<std::pair<Addon, Catalog>> out;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& a : addons) {
            if (a.manifest.resources.count("catalog") == 0) continue;
            for (const auto& c : a.manifest.catalogs) out.emplace_back(a, c);
        }
    }

    // Search adds its query only after allCatalogs() returns. Register URL roots
    // grouped by type; requests::get materializes the concrete /search=... URLs
    // lazily when the first catalog is actually queried.
    std::map<std::string, std::vector<std::string>> searchRoots;
    for (const auto& pc : out) {
        if (!pc.second.hasSearch()) continue;
        searchRoots[pc.second.type].push_back(
            pc.first.base + "/catalog/" + pc.second.type + "/" + encodeURIComponent(pc.second.id));
    }
    for (const auto& group : searchRoots) requests::registerSearchBatch(group.second);
    return out;
}

std::vector<std::pair<Addon, Catalog>> AddonEngine::catalogsForType(const std::string& stremioType) {
    std::vector<std::pair<Addon, Catalog>> out;
    {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& a : addons) {
            if (a.manifest.resources.count("catalog") == 0) continue;
            for (const auto& c : a.manifest.catalogs)
                if (c.browsable && c.type == stremioType) out.emplace_back(a, c);
        }
    }

    // Home/section rows consume these plain catalog URLs one-by-one. Registering
    // is intentionally lazy, so callers that only enumerate tabs cause no I/O.
    if (out.size() > 1) {
        std::vector<std::string> urls;
        urls.reserve(out.size());
        for (const auto& pc : out) urls.push_back(resourceUrl(pc.first, "catalog", pc.second.type, pc.second.id));
        requests::registerBatch(urls);
    }
    return out;
}

std::vector<std::string> AddonEngine::browsableTypes() {
    std::lock_guard<std::mutex> lock(mtx);
    std::vector<std::string> out;
    for (const auto& a : addons) {
        if (a.manifest.resources.count("catalog") == 0) continue;
        for (const auto& c : a.manifest.catalogs) {
            if (!c.browsable) continue;
            if (std::find(out.begin(), out.end(), c.type) == out.end()) out.push_back(c.type);
        }
    }
    return out;
}

std::string AddonEngine::resourceUrl(const Addon& addon, const std::string& resource, const std::string& type,
    const std::string& id, const std::vector<std::pair<std::string, std::string>>& extra) const {
    std::string url = addon.base + "/" + resource + "/" + type + "/" + encodeURIComponent(id);
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

}  // namespace stremio