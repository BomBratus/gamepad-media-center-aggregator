#include <borealis.hpp>
#include "utils/offline_library.hpp"
#include "utils/offline_catalog.hpp"
#include "utils/image_cache.hpp"
#include "utils/config.hpp"
#include "utils/misc.hpp"
#include "utils/download.hpp"

#include <fstream>
#include "utils/serial_writer.hpp"

std::string OfflineLibrary::metaDir() const { return AppConfig::instance().configDir() + "/downloads/meta"; }

std::string OfflineLibrary::metaPath(const std::string& ratingKey) const {
    // never reach disk). The authoritative ratingKey is read back from the JSON
    // content, not the filename.
    std::string safe;
    for (char c : ratingKey)
        safe += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return this->metaDir() + "/" + safe + ".json";
}

void OfflineLibrary::init() {
    std::string dir = this->metaDir();
    if (!fs::exists(dir)) {
        try {
            fs::create_directories(dir);
        } catch (const std::exception& e) {
            brls::Logger::error("OfflineLibrary: cannot create {}: {}", dir, e.what());
        }
    }
    {
        std::lock_guard<std::mutex> lock(this->mutex);
        this->load();
        this->rebuild();
    }
    // legacy back-fill takes the lock per putItem, so run it outside the block
    this->migrateLegacy();
}

void OfflineLibrary::load() {
    this->nodes.clear();
    std::string dir = this->metaDir();
    if (!fs::exists(dir)) return;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().extension().string() != ".json") continue;
        try {
            std::ifstream f(entry.path().string());
            nlohmann::json j = nlohmann::json::parse(f);
            this->nodes.push_back(j.get<media::Item>());
        } catch (const std::exception& e) {
            brls::Logger::error("OfflineLibrary: bad meta {}: {}", entry.path().string(), e.what());
        }
    }
}

void OfflineLibrary::rebuild() {
    ++this->revision;
    this->derived = offline::synthesizeAncestors(this->nodes);
}

void OfflineLibrary::writeMeta(const media::Item& item) const {
    const auto path = this->metaPath(item.ratingKey);
    filePersistence().submit([path, item] {
        try {
            nlohmann::json j = item;
            std::ofstream f(path + ".tmp");
            f << j.dump(2); f.close();
            if (!f || std::rename((path + ".tmp").c_str(), path.c_str()) != 0)
                throw std::runtime_error("Cannot publish offline metadata");
        } catch (const std::exception& e) {
            brls::Logger::error("OfflineLibrary: cannot write meta {}: {}", item.ratingKey, e.what());
        }
    }, path);
}

void OfflineLibrary::putItem(const media::Item& item) {
    if (item.ratingKey.empty()) return;
    std::lock_guard<std::mutex> lock(this->mutex);
    this->writeMeta(item);
    bool replaced = false;
    for (auto& n : this->nodes) {
        if (n.ratingKey == item.ratingKey) {
            n = item;
            replaced = true;
            break;
        }
    }
    if (!replaced) this->nodes.push_back(item);
    this->rebuild();
}

bool OfflineLibrary::hasItem(const std::string& ratingKey) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& n : this->nodes)
        if (n.ratingKey == ratingKey) return true;
    return false;
}

bool OfflineLibrary::getItem(const std::string& ratingKey, media::Item& out) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    for (auto& n : this->derived) {
        if (n.ratingKey == ratingKey) {
            out = n;
            return true;
        }
    }
    return false;
}

std::vector<media::Section> OfflineLibrary::sections() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return offline::buildSections(this->derived);
}

std::vector<media::Item> OfflineLibrary::sectionItems(const std::string& sectionKey) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return offline::sectionItems(this->derived, sectionKey);
}

std::vector<media::Item> OfflineLibrary::children(const std::string& ratingKey) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return offline::childrenOf(this->derived, ratingKey);
}

std::vector<media::Item> OfflineLibrary::leaves(const std::string& showRatingKey) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return offline::leavesOf(this->derived, showRatingKey);
}

std::vector<media::Item> OfflineLibrary::search(const std::string& term) const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return offline::search(this->derived, term);
}

bool OfflineLibrary::empty() const {
    std::lock_guard<std::mutex> lock(this->mutex);
    return offline::buildSections(this->derived).empty();
}

void OfflineLibrary::removeItem(const std::string& ratingKey) {
    std::lock_guard<std::mutex> lock(this->mutex);
    std::vector<std::string> assets;
    for (const auto& n : this->nodes)
        if (n.ratingKey == ratingKey) { assets = offline::assetPaths(n); break; }
    const auto path = metaPath(ratingKey);
    filePersistence().submit([path, assets] {
        try { if (fs::exists(path)) fs::remove(path); }
        catch (const std::exception& e) { brls::Logger::warning("Offline removal: {}", e.what()); }
        for (const auto& asset : assets) ImageCache::remove(asset);
    });
    this->nodes.erase(std::remove_if(this->nodes.begin(), this->nodes.end(),
                          [&](const media::Item& n) { return n.ratingKey == ratingKey; }),
        this->nodes.end());
    this->rebuild();
}

void OfflineLibrary::prune() {
    // Never call DownloadManager under our mutex. Publish against the same
    // local revision that was inspected, retrying if a concurrent put/remove
    // changed the tree while we queried file state.
    std::unique_lock<std::mutex> lock(this->mutex, std::defer_lock);
    std::unordered_set<std::string> keep;
    for (;;) {
        lock.lock();
        const auto version = this->revision;
        const auto snapshot = this->nodes;
        lock.unlock();
        keep = offline::survivors(snapshot, [](const std::string& k) {
            return DownloadManager::instance().isDownloaded(k);
        });
        lock.lock();
        if (version == this->revision) break;
        lock.unlock();
    }

    std::vector<media::Item> kept;
    kept.reserve(this->nodes.size());
    for (auto& n : this->nodes) {
        if (keep.count(n.ratingKey)) {
            kept.push_back(std::move(n));
            continue;
        }
        const auto path = this->metaPath(n.ratingKey);
        const auto assets = offline::assetPaths(n);
        filePersistence().submit([path, assets] {
            try { if (fs::exists(path)) fs::remove(path); } catch (...) {}
            for (const auto& asset : assets) ImageCache::remove(asset);
        });
    }
    this->nodes = std::move(kept);
    this->rebuild();
}

void OfflineLibrary::migrateLegacy() {
    std::string index = AppConfig::instance().configDir() + "/downloads/index.json";
    if (!fs::exists(index)) return;
    std::vector<DownloadItem> items;
    try {
        std::ifstream f(index);
        nlohmann::json j = nlohmann::json::parse(f);
        items = j.get<std::vector<DownloadItem>>();
    } catch (const std::exception& e) {
        brls::Logger::error("OfflineLibrary: cannot read legacy index: {}", e.what());
        return;
    }
    for (auto& dl : items) {
        // only completed downloads are browsable; skip if already captured
        if (dl.status != DownloadStatus::Completed) continue;
        if (this->hasItem(dl.itemId)) continue;

        media::Item it;
        it.ratingKey = dl.itemId;
        it.title = dl.name;
        it.year = dl.productionYear;
        it.duration = dl.durationMs;
        it.thumb = dl.thumb;
        if (dl.type == media::mediaTypeEpisode) {
            it.type = media::mediaTypeEpisode;
            it.index = dl.episodeIndex;
            it.parentIndex = dl.seasonIndex;
            it.grandparentTitle = dl.seriesName;
        } else {
            // movie or clip -> browsable as a top-level movie
            it.type = media::mediaTypeMovie;
        }
        this->putItem(it);
    }
}
