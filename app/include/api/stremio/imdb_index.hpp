#pragma once

#include "api/stremio/archive_model.hpp"
#include "api/stremio/archive_cursor.hpp"
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
    Cursor cursor;
    QueryMetrics metrics;
};
using IndexCancel = std::shared_ptr<std::atomic_bool>;
// Serial worker only. Read-only connection and immutable indexes pin the old
// inode across publication. Strings/aliases/search postings stay on disk.
class ImdbIndex {
public:
    explicit ImdbIndex(const std::string& path);
    ~ImdbIndex();
    ImdbIndex(const ImdbIndex&) = delete;
    ImdbIndex& operator=(const ImdbIndex&) = delete;
    IndexResult query(const Filter&, const Cursor&, size_t limit, bool random, const IndexCancel& cancel = {});
private:
    struct Engine;
    std::unique_ptr<Engine> engine;
};
using IndexYield = std::function<void()>;
using DatasetDownload = std::function<void(const std::string& name, const std::string& path, const IndexCancel&)>;
// Full staging rebuild; resumable import, cancellable derived indexes. No live
// mutations of published generations. Publish only after structural validation.
bool buildImdbIndex(const std::string& path, const IndexCancel&, const DatasetDownload&,
    const std::function<void(size_t)>& progress = {}, const IndexYield& yield = {});
} // namespace stremio::archive
