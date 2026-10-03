#pragma once

#include "api/stremio/archive_model.hpp"
#include <atomic>
#include <functional>
#include <memory>

struct sqlite3;
namespace stremio::archive {

struct IndexResult {
    std::vector<Record> records;
    std::vector<std::string> genres;
    size_t total = 0, indexed = 0;
    int64_t refreshed = 0;
};

using IndexCancel = std::shared_ptr<std::atomic_bool>;

// Read-only connections hold the old file across an atomic refresh replacement.
class ImdbIndex {
public:
    explicit ImdbIndex(const std::string& path);
    ~ImdbIndex();
    ImdbIndex(const ImdbIndex&) = delete;
    ImdbIndex& operator=(const ImdbIndex&) = delete;
    IndexResult query(const Filter&, size_t offset, size_t limit, bool random, const IndexCancel& cancel = {});
private:
    sqlite3* db = nullptr;
    size_t count = 0;
    int64_t refreshed = 0;
    std::vector<std::string> genreOptions;
    // Disk-backed search IDs and filtered rows belong to this generation.
    // Paging, sorting and Random reuse them without scanning titles/aliases.
    std::string cachedSearch;
    bool searchReady = false, resultReady = false;
    Filter cachedFilter;
    size_t cachedTotal = 0;
};

using DatasetDownload = std::function<void(const std::string& name, const std::string& path, const IndexCancel&)>;
// Builds on disk in bounded batches. Returns false on playback cancellation;
// committed rows/offsets and completed gzip downloads survive for the next pass.
bool buildImdbIndex(const std::string& path, const IndexCancel&, const DatasetDownload&,
    const std::function<void(size_t)>& progress = {});

} // namespace stremio::archive
