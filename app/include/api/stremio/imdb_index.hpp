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

// Read-only connections hold the old file across an atomic refresh replacement.
class ImdbIndex {
public:
    explicit ImdbIndex(const std::string& path);
    ~ImdbIndex();
    ImdbIndex(const ImdbIndex&) = delete;
    ImdbIndex& operator=(const ImdbIndex&) = delete;
    IndexResult query(const Filter&, size_t offset, size_t limit, bool random);
private:
    sqlite3* db = nullptr;
    size_t count = 0;
    int64_t refreshed = 0;
    std::vector<std::string> genreOptions;
};

using IndexCancel = std::shared_ptr<std::atomic_bool>;
using DatasetDownload = std::function<void(const std::string& name, const std::string& path, const IndexCancel&)>;
// Builds on disk in bounded batches. Returns false on playback cancellation;
// committed rows/offsets and completed gzip downloads survive for the next pass.
bool buildImdbIndex(const std::string& path, const IndexCancel&, const DatasetDownload&,
    const std::function<void(size_t)>& progress = {});

} // namespace stremio::archive
