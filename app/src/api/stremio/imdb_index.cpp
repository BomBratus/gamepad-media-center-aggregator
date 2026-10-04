#include "api/stremio/imdb_index.hpp"
#include <sqlite3.h>
#include <zlib.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <map>
#include <string_view>
#if defined(__PS4__)
#include "utils/ps4_diagnostics.hpp"
#endif

namespace stremio::archive {
namespace {
struct InvalidDataset : std::runtime_error { using std::runtime_error::runtime_error; };

#if defined(__PS4__) || defined(GMCA_INDEX_NOLOCK)
const char* privateIndexVfs() {
    static sqlite3_vfs vfs{};
    static std::once_flag registered;
    std::call_once(registered, [] {
        auto base = sqlite3_vfs_find("unix-none");
        if (!base) throw std::runtime_error("SQLite PS4 VFS unavailable");
        vfs = *base;
        vfs.pNext = nullptr;
        vfs.zName = "gmca-ps4-index";
        // All index/journal paths are absolute and generated under the writable
        // app directory. OpenOrbis lstat/readlink are ENOSYS; unix-none still
        // calls them during canonicalization even though locking is disabled.
        vfs.xFullPathname = [](sqlite3_vfs*, const char* path, int size, char* out) {
            if (!path || path[0] != '/' || size <= 0) return SQLITE_CANTOPEN;
            auto length = std::strlen(path);
            if (length >= static_cast<size_t>(size)) return SQLITE_CANTOPEN;
            std::memcpy(out, path, length + 1);
            return SQLITE_OK;
        };
        if (sqlite3_vfs_register(&vfs, 0) != SQLITE_OK)
            throw std::runtime_error("Cannot register SQLite PS4 VFS");
    });
    return vfs.zName;
}
#endif

struct Database {
    sqlite3* db = nullptr;
    Database(const std::string& path, int flags) {
#if defined(__PS4__) || defined(GMCA_INDEX_NOLOCK)
        static std::once_flag temporaryDirectory;
        std::call_once(temporaryDirectory, [&] {
            sqlite3_temp_directory = sqlite3_mprintf("%s", path.substr(0, path.find_last_of('/')).c_str());
        });
#endif
        // There is exactly one staging writer; published generations are never
        // modified. PS4 needs no POSIX byte-range locks for this file lifecycle.
        const char* vfs = nullptr;
#if defined(__PS4__) || defined(GMCA_INDEX_NOLOCK)
        vfs = privateIndexVfs();
#endif
        int code = sqlite3_open_v2(path.c_str(), &db, flags, vfs);
        if (code != SQLITE_OK) {
            std::string detail = "Cannot open IMDb index: SQLite code " + std::to_string(code);
            if (db) sqlite3_close(db);
            throw std::runtime_error(detail);
        }
        sqlite3_busy_timeout(db, 3000);
    }
    ~Database() { if (db) sqlite3_close(db); }
};
void exec(sqlite3* db, const char* sql) {
    int code = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    if (code != SQLITE_OK)
        throw std::runtime_error("Cannot update IMDb index: SQLite code " + std::to_string(sqlite3_extended_errcode(db)));
}
struct Statement {
    sqlite3_stmt* stmt = nullptr;
    QueryMetrics* metrics = nullptr;
    Statement(sqlite3* db, const std::string& sql, QueryMetrics* audit = nullptr) : metrics(audit) {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error("Invalid IMDb query");
    }
    ~Statement() { sqlite3_finalize(stmt); }
    void text(int i, std::string_view value) {
        if (sqlite3_bind_text(stmt, i, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            throw std::runtime_error("Cannot bind IMDb query");
    }
    void number(int i, int64_t value) { sqlite3_bind_int64(stmt, i, value); }
    void real(int i, double value) { sqlite3_bind_double(stmt, i, value); }
    bool row() {
        int result = sqlite3_step(stmt);
        if (metrics) {
            metrics->sqlFullScanSteps += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, 1);
            metrics->sqlSorts += sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_SORT, 1);
        }
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) throw std::runtime_error("Cannot run IMDb query");
        return false;
    }
    void run() { row(); sqlite3_reset(stmt); sqlite3_clear_bindings(stmt); }
    std::string text(int i) {
        auto value = sqlite3_column_text(stmt, i);
        return value ? reinterpret_cast<const char*>(value) : "";
    }
    int64_t number(int i) { return sqlite3_column_int64(stmt, i); }
    double real(int i) { return sqlite3_column_double(stmt, i); }
};

struct QueryCancellation {
    sqlite3* db = nullptr;
    IndexCancel cancel;
    QueryCancellation(sqlite3* value, IndexCancel token) : db(value), cancel(std::move(token)) {
        if (!cancel) return;
        sqlite3_progress_handler(db, 1000, [](void* value) {
            return static_cast<std::atomic_bool*>(value)->load() ? 1 : 0;
        }, cancel.get());
    }
    ~QueryCancellation() {
        if (cancel) sqlite3_progress_handler(db, 0, nullptr, nullptr);
    }
};
int64_t setting(sqlite3* db, const char* key) {
    Statement stmt(db, "SELECT value FROM settings WHERE key=?");
    stmt.text(1, key);
    return stmt.row() ? stmt.number(0) : 0;
}
void setting(sqlite3* db, const char* key, int64_t value) {
    Statement stmt(db, "INSERT OR REPLACE INTO settings VALUES(?,?)");
    stmt.text(1, key); stmt.number(2, value); stmt.run();
}
std::vector<std::string_view> split(const std::string& line, char delimiter = '\t') {
    std::vector<std::string_view> fields;
    size_t start = 0;
    for (;;) {
        auto end = line.find(delimiter, start);
        fields.emplace_back(line.data() + start, (end == std::string::npos ? line.size() : end) - start);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return fields;
}
int64_t integer(std::string_view value) {
    int64_t result = 0;
    if (value.empty() || value.size() > 12) return 0;
    for (auto c : value) {
        if (c < '0' || c > '9') return 0;
        result = result * 10 + c - '0';
    }
    return result;
}
struct Gzip {
    gzFile file = nullptr;
    explicit Gzip(const std::string& path) : file(gzopen(path.c_str(), "rb")) {
        if (!file) throw std::runtime_error("Cannot open IMDb dataset");
        gzbuffer(file, 64 * 1024);
    }
    ~Gzip() { gzclose(file); }
    bool line(std::string& out, const IndexYield& yield = {}) {
        out.clear();
        std::array<char, 4096> buffer;
        while (gzgets(file, buffer.data(), buffer.size())) {
            if (yield && out.size() >= 64 * 1024) yield();
            out += buffer.data();
            if (!out.empty() && out.back() == '\n') {
                out.pop_back();
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            if (out.size() > 1024 * 1024) throw InvalidDataset("Invalid IMDb row");
        }
        int error = Z_OK;
        gzerror(file, &error);
        if (error != Z_OK && error != Z_STREAM_END) throw InvalidDataset("Truncated IMDb dataset");
        return !out.empty();
    }
};
int64_t timestamp() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}
} // namespace

namespace {
using Bits = std::vector<uint64_t>;
constexpr size_t maxTitles = 2000000, maxGenres = 64, postingChunk = 16384;
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void checkCancel(const IndexCancel& cancel) {
    if (cancel && cancel->load()) throw std::runtime_error("Archive query interrupted");
}
struct BuildWork {
    IndexCancel cancel;
    const IndexYield& yield;
    const BuildTiming& timing;
    void measured(const std::string& phase, Clock::time_point started) const {
        const auto ms = elapsed(started);
#if defined(__PS4__)
        ps4diag::write("archive build timing phase=" + phase + " ms=" + std::to_string(ms));
#endif
        if (timing) timing(phase, ms);
    }
    void checkpoint() const {
        if (yield) yield();
        checkCancel(cancel);
    }
    static int sqliteProgress(void* context) noexcept {
        try { static_cast<BuildWork*>(context)->checkpoint(); return 0; }
        catch (...) { return 1; }
    }
};
Bits universe(size_t n) {
    Bits bits((n + 63) / 64, ~uint64_t(0));
    if (n % 64) bits.back() = (uint64_t(1) << (n % 64)) - 1;
    return bits;
}
void setBit(Bits& bits, uint32_t key) { bits[key / 64] |= uint64_t(1) << (key % 64); }
bool hasBit(const Bits& bits, uint32_t key) { return bits[key / 64] & (uint64_t(1) << (key % 64)); }
template<class T> std::vector<unsigned char> encode(const std::vector<T>& values, const IndexYield& yield = {}) {
    std::vector<unsigned char> data(values.size() * sizeof(T));
    for (size_t i = 0; i < values.size(); ++i) {
        if (yield && !(i % 4096)) yield();
        for (size_t b = 0; b < sizeof(T); ++b) data[i * sizeof(T) + b] = values[i] >> (8 * b);
    }
    return data;
}
template<class T> std::vector<T> decode(sqlite3_stmt* stmt, int column) {
    auto size = sqlite3_column_bytes(stmt, column);
    auto data = static_cast<const unsigned char*>(sqlite3_column_blob(stmt, column));
    if (size < 0 || size % sizeof(T) || (size && !data)) throw std::runtime_error("Invalid Archive blob");
    std::vector<T> values(size / sizeof(T));
    for (size_t i = 0; i < values.size(); ++i)
        for (size_t b = 0; b < sizeof(T); ++b) values[i] |= T(data[i * sizeof(T) + b]) << (8 * b);
    return values;
}
void blob(Statement& stmt, int column, const std::vector<unsigned char>& data) {
    if (sqlite3_bind_blob(stmt.stmt, column, data.data(), static_cast<int>(data.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("Cannot bind Archive blob");
}
bool samePredicates(const Filter& a, const Filter& b) {
    return lower(a.search) == lower(b.search) && a.type == b.type && lower(a.genre) == lower(b.genre) &&
        a.yearFrom == b.yearFrom && a.yearTo == b.yearTo && a.minRating == b.minRating &&
        a.minVotes == b.minVotes && a.country == b.country && a.service == b.service &&
        a.addon == b.addon && a.minViews == b.minViews && a.other == b.other;
}
std::vector<std::string> grams(const std::string& text, size_t width) {
    std::vector<std::string> result;
    if (text.size() >= width)
        for (size_t i = 0; i + width <= text.size(); ++i) result.push_back(text.substr(i, width));
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
std::string sortName(Sort sort) {
    switch (sort) {
        case Sort::Name: return "name";
        case Sort::Rating: return "rating";
        case Sort::Votes: return "votes";
        case Sort::Added: case Sort::Updated: return "id";
        default: return "year";
    }
}
} // namespace

struct MatchState {
    uint64_t generation = 0;
    Filter filter;
    Bits bits;
    std::vector<uint32_t> keys; // sorted by title_key; Random directly samples this
};
struct ImdbIndex::Engine {
    sqlite3* db = nullptr;
    size_t count = 0;
    int64_t refreshed = 0;
    uint64_t generation = 0;
    std::vector<std::string> genres;
    std::map<std::string, Bits> facets;
    std::shared_ptr<const MatchState> lastMatch;
    std::string loadedSort;
    std::vector<uint32_t> order, ends;
    ~Engine() { if (db) sqlite3_close(db); }
    const Bits& facet(const std::string& name, QueryMetrics& metrics) {
        auto found = facets.find(name);
        if (found != facets.end()) return found->second;
        Statement row(db, "SELECT data FROM facets WHERE name=?", &metrics); row.text(1, name);
        Bits bits((count + 63) / 64);
        if (row.row()) bits = decode<uint64_t>(row.stmt, 0);
        if (bits.size() != (count + 63) / 64) throw std::runtime_error("Invalid Archive facet");
        return facets.emplace(name, std::move(bits)).first->second;
    }
    // Bit-sliced >= comparator: no title records, numeric sort, or value scan.
    Bits atLeast(const char* field, unsigned width, uint64_t threshold, QueryMetrics& metrics, const IndexCancel& cancel) {
        Bits equal = universe(count), greater(equal.size());
        if (threshold >= (uint64_t(1) << width)) return greater;
        for (unsigned b = width; b-- > 0;) {
            checkCancel(cancel);
            const auto& plane = facet(std::string(field) + ":" + std::to_string(b), metrics);
            for (size_t i = 0; i < equal.size(); ++i) {
                if ((threshold >> b) & 1) equal[i] &= plane[i];
                else { greater[i] |= equal[i] & plane[i]; equal[i] &= ~plane[i]; }
            }
            metrics.facetWords += equal.size();
        }
        for (size_t i = 0; i < equal.size(); ++i) greater[i] |= equal[i];
        return greater;
    }
    void loadSort(const std::string& name, const IndexCancel& cancel, QueryMetrics& metrics) {
        if (name == loadedSort) return;
        checkCancel(cancel);
        // Release previous arrays before loading another order; four arrays
        // exist on disk, only one (+ tie boundaries) resides in the heap.
        std::vector<uint32_t>().swap(order); std::vector<uint32_t>().swap(ends);
        loadedSort.clear();
        if (name != "id") {
            Statement row(db, "SELECT keys,ends FROM sort_orders WHERE name=?", &metrics); row.text(1, name);
            if (!row.row()) throw std::runtime_error("Missing Archive sort");
            order = decode<uint32_t>(row.stmt, 0); ends = decode<uint32_t>(row.stmt, 1);
            if (order.size() != count || ends.empty() || ends.back() != count || ends.front() == 0 ||
                !std::is_sorted(ends.begin(), ends.end()) || std::adjacent_find(ends.begin(), ends.end()) != ends.end())
                throw std::runtime_error("Invalid Archive sort");
        }
        checkCancel(cancel);
        loadedSort = name;
    }

};
ImdbIndex::ImdbIndex(const std::string& path) : engine(std::make_unique<Engine>()) {
    Database connection(path, SQLITE_OPEN_READONLY);
    if (setting(connection.db, "version") != 2 || !setting(connection.db, "complete"))
        throw std::runtime_error("Archive format requires rebuild");
    engine->count = setting(connection.db, "browsable");
    if (!engine->count || engine->count > maxTitles) throw std::runtime_error("Invalid Archive count");
    engine->refreshed = setting(connection.db, "refreshed");
    static std::atomic<uint64_t> next{1};
    engine->generation = next.fetch_add(1);
    Statement genres(connection.db, "SELECT name FROM genre_options ORDER BY name");
    while (genres.row()) engine->genres.push_back(genres.text(0));
    if (engine->genres.size() > maxGenres) throw std::runtime_error("Too many Archive genres");
    engine->db = connection.db; connection.db = nullptr;
}
ImdbIndex::~ImdbIndex() = default;

IndexResult ImdbIndex::query(const Filter& filter, const Cursor& cursor, size_t limit, bool random, const IndexCancel& cancel) {
    auto& e = *engine;
    IndexResult result;
    result.indexed = e.count; result.refreshed = e.refreshed; result.genres = e.genres;
    if (!limit && !random) return result;
    const auto started = Clock::now();
    checkCancel(cancel);
    QueryCancellation cancellation(e.db, cancel);
    const auto filterStarted = Clock::now();
    std::shared_ptr<const MatchState> match;
    if (cursor.matches) {
        if (cursor.generation != e.generation || cursor.matches->generation != e.generation ||
            !samePredicates(cursor.matches->filter, filter) || cursor.sort != static_cast<int>(filter.sort) ||
            cursor.descending != filter.descending || cursor.position > e.count)
            throw std::runtime_error("Invalid Archive cursor");
        match = cursor.matches;
    } else if (e.lastMatch && samePredicates(e.lastMatch->filter, filter)) match = e.lastMatch;
    result.metrics.reusedMatches = bool(match);
    if (!match) {
        // One active UI session. Drop the cache before building a different set.
        e.lastMatch.reset();
        auto next = std::make_shared<MatchState>();
        next->generation = e.generation; next->filter = filter;
        next->bits = universe(e.count);
        auto intersect = [&](const Bits& bits, bool invert = false) {
            checkCancel(cancel);
            for (size_t i = 0; i < bits.size(); ++i) next->bits[i] &= invert ? ~bits[i] : bits[i];
            result.metrics.facetWords += bits.size();
        };
        if (!filter.country.empty() || !filter.service.empty() || !filter.addon.empty() || filter.minViews || filter.other == 2)
            std::fill(next->bits.begin(), next->bits.end(), 0);
        if (!filter.type.empty()) {
            if (filter.type != "movie" && filter.type != "series") std::fill(next->bits.begin(), next->bits.end(), 0);
            else intersect(e.facet("type:" + filter.type, result.metrics));
        }
        if (!filter.genre.empty()) {
            const auto wanted = lower(filter.genre);
            auto found = std::find_if(e.genres.begin(), e.genres.end(), [&](const std::string& s) { return lower(s) == wanted; });
            if (found == e.genres.end()) std::fill(next->bits.begin(), next->bits.end(), 0);
            else intersect(e.facet("genre:" + wanted, result.metrics));
        }
        if (filter.yearFrom) intersect(e.atLeast("year", 16, std::max<int64_t>(0, filter.yearFrom), result.metrics, cancel));
        if (filter.yearTo) {
            intersect(e.atLeast("year", 16, 1, result.metrics, cancel)); // unknown year excluded
            intersect(e.atLeast("year", 16, filter.yearTo < 0 ? 0 : uint64_t(filter.yearTo) + 1, result.metrics, cancel), true);
        }
        if (filter.minRating) {
            const double threshold = std::ceil(filter.minRating * 10 - 1e-9);
            intersect(e.atLeast("rating", 7, !std::isfinite(threshold) || threshold > 127 ? 128 : std::max(0.0, threshold), result.metrics, cancel));
        }
        if (filter.minVotes) intersect(e.atLeast("votes", 32, std::max<int64_t>(0, filter.minVotes), result.metrics, cancel));
        const auto search = lower(filter.search);
        if (!search.empty()) {
            const auto searchStarted = Clock::now();
            auto terms = grams(search, std::min<size_t>(3, search.size()));
            std::vector<std::pair<size_t, std::string>> ranked;
            Statement sizes(e.db, "SELECT count FROM search_terms WHERE term=?", &result.metrics);
            for (const auto& term : terms) {
                sizes.text(1, term);
                auto n = sizes.row() ? sizes.number(0) : 0;
                ranked.emplace_back(n, term);
                sqlite3_reset(sizes.stmt); sqlite3_clear_bindings(sizes.stmt);
            }
            std::sort(ranked.begin(), ranked.end());
            std::vector<uint32_t> candidates;
            bool first = true;
            for (const auto& term : ranked) {
                checkCancel(cancel);
                if (!first && candidates.empty()) break;
                std::vector<uint32_t> selected;
                selected.reserve(first ? std::min(term.first, e.count) : candidates.size());
                Statement postings(e.db, "SELECT data FROM search_postings WHERE term=? ORDER BY chunk", &result.metrics);
                postings.text(1, term.second);
                size_t at = 0;
                while (postings.row()) {
                    checkCancel(cancel);
                    auto keys = decode<uint32_t>(postings.stmt, 0);
                    result.metrics.searchPostings += keys.size();
                    for (auto key : keys) {
                        if (key >= e.count) throw std::runtime_error("Invalid Archive posting");
                        if (!hasBit(next->bits, key)) continue;
                        if (!first) {
                            while (at < candidates.size() && candidates[at] < key) ++at;
                            if (at == candidates.size() || candidates[at] != key) continue;
                        }
                        selected.push_back(key);
                    }
                }
                candidates.swap(selected); first = false;
            }
            Bits searched(next->bits.size());
            // Verify contiguous occurrence in bounded sequential text blocks,
            // not one SQLite/name seek per candidate. Buckets cover 256 keys;
            // even a very common query needs ~N/256 seeks rather than N.
            Bits wanted(next->bits.size());
            for (auto key : candidates) setBit(wanted, key);
            result.metrics.searchCandidates = candidates.size();
            if (search.size() <= 3) searched.swap(wanted);
            else {
                Statement verify(e.db, "SELECT data FROM search_text WHERE bucket=? ORDER BY chunk", &result.metrics);
                size_t at = 0;
                while (at < candidates.size()) {
                    checkCancel(cancel);
                    const auto bucket = candidates[at] / 256;
                    verify.number(1, bucket);
                    bool blockFound = false;
                    while (verify.row()) {
                        checkCancel(cancel); blockFound = true;
                        const auto size = sqlite3_column_bytes(verify.stmt, 0);
                        auto data = static_cast<const unsigned char*>(sqlite3_column_blob(verify.stmt, 0));
                        if (!data || size < 0 || size > 1024 * 1024) throw std::runtime_error("Invalid Archive text block");
                        size_t position = 0;
                        auto word = [&] {
                            if (position + 4 > size_t(size)) throw std::runtime_error("Truncated Archive text block");
                            uint32_t value = 0;
                            for (unsigned b = 0; b < 4; ++b) value |= uint32_t(data[position++]) << (8 * b);
                            return value;
                        };
                        while (position < size_t(size)) {
                            const auto key = word(), length = word();
                            if (key >= e.count || key / 256 != bucket || length > 4096 || length > size_t(size) - position)
                                throw std::runtime_error("Invalid Archive text reference");
                            if (hasBit(wanted, key) && !hasBit(searched, key) &&
                                std::string_view(reinterpret_cast<const char*>(data + position), length).find(search) != std::string_view::npos)
                                setBit(searched, key);
                            position += length;
                        }
                    }
                    if (!blockFound) throw std::runtime_error("Missing Archive text block");
                    sqlite3_reset(verify.stmt); sqlite3_clear_bindings(verify.stmt);
                    while (at < candidates.size() && candidates[at] / 256 == bucket) ++at;
                }
            }
            next->bits.swap(searched);
            result.metrics.searchMs = elapsed(searchStarted);
        }
        // Enumerate set bits, not title records. Cached for total + uniform Random.
        size_t matched = 0;
        for (auto word : next->bits) matched += __builtin_popcountll(word);
        next->keys.reserve(matched);
        for (size_t w = 0; w < next->bits.size(); ++w) {
            if (!(w % 1024)) checkCancel(cancel);
            auto word = next->bits[w];
            while (word) {
                unsigned bit = __builtin_ctzll(word);
                const auto key = w * 64 + bit;
                if (key >= e.count) throw std::runtime_error("Invalid Archive facet padding");
                next->keys.push_back(static_cast<uint32_t>(key));
                word &= word - 1;
            }
        }
        checkCancel(cancel);
        match = next; e.lastMatch = match;
    }
    result.metrics.filterMs = elapsed(filterStarted) - result.metrics.searchMs;
    result.total = match->keys.size();
    result.cursor = {match, e.generation, cursor.position, static_cast<int>(filter.sort), filter.descending};
    std::vector<uint32_t> visible;
    const auto sortStarted = Clock::now();
    if (random && result.total) {
        // Predictable platform support: OpenOrbis need not expose /dev/urandom.
        static std::mt19937 rng(static_cast<uint32_t>(timestamp()));
        visible.push_back(match->keys[std::uniform_int_distribution<size_t>(0, result.total - 1)(rng)]);
    } else if (result.total) {
        e.loadSort(sortName(filter.sort), cancel, result.metrics);
        size_t group = e.ends.size(), groupStart = 0, groupEnd = 0;
        if (filter.descending && e.loadedSort != "id" && result.cursor.position < e.count) {
            group = std::upper_bound(e.ends.begin(), e.ends.end(), e.count - 1 - result.cursor.position) - e.ends.begin();
            groupEnd = e.ends[group]; groupStart = group ? e.ends[group - 1] : 0;
        }
        while (result.cursor.position < e.count && visible.size() < limit) {
            if (!(result.metrics.sortEntries % 1024)) checkCancel(cancel);
            const auto position = result.cursor.position++;
            uint32_t key;
            if (e.loadedSort == "id") key = filter.descending ? e.count - 1 - position : position;
            else if (!filter.descending) key = e.order[position];
            else {
                // Reverse groups, preserve IMDb-ID tie order. One boundary
                // lookup per page, then sequential array traversal per group.
                if (position >= e.count - groupStart) {
                    --group; groupEnd = e.ends[group]; groupStart = group ? e.ends[group - 1] : 0;
                }
                key = e.order[groupStart + position - (e.count - groupEnd)];
            }
            ++result.metrics.sortEntries;
            if (key >= e.count) throw std::runtime_error("Invalid Archive sort key");
            if (hasBit(match->bits, key)) visible.push_back(key);
        }
    } else result.cursor.position = e.count;
    result.metrics.sortMs = elapsed(sortStarted);
    const auto metadataStarted = Clock::now();
    Statement rows(e.db, "SELECT id,kind,name,year,rating,votes FROM records WHERE title_key=?", &result.metrics);
    for (auto key : visible) {
        checkCancel(cancel); rows.number(1, key);
        if (!rows.row()) throw std::runtime_error("Missing Archive metadata");
        Record record;
        record.meta = {{"id", rows.text(0)}, {"type", rows.text(1)}, {"name", rows.text(2)},
            {"year", std::to_string(rows.number(3))}, {"releaseInfo", std::to_string(rows.number(3))},
            {"imdbRating", rows.real(4)}, {"votes", rows.number(5)},
            {"poster", "https://images.metahub.space/poster/small/" + rows.text(0) + "/img"}};
        record.added = record.updated = e.refreshed;
        result.records.push_back(std::move(record)); ++result.metrics.metadataRows;
        sqlite3_reset(rows.stmt); sqlite3_clear_bindings(rows.stmt);
    }
    result.metrics.metadataMs = elapsed(metadataStarted); result.metrics.totalMs = elapsed(started);
    return result;
}

namespace {
void buildLog(const std::string& phase, size_t titles, const std::string& index, Clock::time_point start) {
#if defined(__PS4__)
    ps4diag::write("archive build phase=" + phase + " titles=" + std::to_string(titles) +
        " index=" + index + " elapsed-ms=" + std::to_string(elapsed(start)));
#else
    (void)phase; (void)titles; (void)index; (void)start;
#endif
}
void buildDerived(sqlite3* db, const BuildWork& work, const std::function<void(size_t)>& progress) {
    const auto started = Clock::now();
    exec(db, "DROP TABLE IF EXISTS records; DROP TABLE IF EXISTS facets; DROP TABLE IF EXISTS genre_options;"
        "DROP TABLE IF EXISTS sort_orders; DROP TABLE IF EXISTS title_search; DROP TABLE IF EXISTS search_pairs;"
        "DROP TABLE IF EXISTS search_terms; DROP TABLE IF EXISTS search_postings; DROP TABLE IF EXISTS search_text;"
        "CREATE TABLE records(title_key INTEGER PRIMARY KEY,id TEXT UNIQUE,kind TEXT,name TEXT,year INTEGER,rating REAL,votes INTEGER,score REAL);"
        "CREATE TABLE facets(name TEXT PRIMARY KEY,data BLOB) WITHOUT ROWID;"
        "CREATE TABLE genre_options(name TEXT PRIMARY KEY) WITHOUT ROWID;"
        "CREATE TABLE sort_orders(name TEXT PRIMARY KEY,keys BLOB,ends BLOB) WITHOUT ROWID;"
        "CREATE TABLE title_search(title_key INTEGER,name TEXT,PRIMARY KEY(title_key,name)) WITHOUT ROWID;"
        "CREATE TABLE search_pairs(term TEXT,title_key INTEGER);"
        "CREATE TABLE search_terms(term TEXT PRIMARY KEY,count INTEGER) WITHOUT ROWID;"
        "CREATE TABLE search_text(bucket INTEGER,chunk INTEGER,data BLOB,PRIMARY KEY(bucket,chunk)) WITHOUT ROWID;"
        "CREATE TABLE search_postings(term TEXT,chunk INTEGER,data BLOB,PRIMARY KEY(term,chunk)) WITHOUT ROWID;"
        "INSERT INTO genre_options SELECT DISTINCT genre FROM genres g JOIN titles t ON t.id=g.id WHERE t.adult=0;");
    Statement total(db, "SELECT count(*) FROM titles WHERE adult=0"); total.row();
    const auto count = static_cast<size_t>(total.number(0));
    if (!count || count > maxTitles) throw std::runtime_error("Archive title budget exceeded");
    std::map<std::string, Bits> facets;
    auto addFacet = [&](const std::string& name) { facets.emplace(name, Bits((count + 63) / 64)); };
    for (auto type : {"movie", "series"}) addFacet(std::string("type:") + type);
    for (auto spec : {std::make_pair("year", 16), {"rating", 7}, {"votes", 32}})
        for (int bit = 0; bit < spec.second; ++bit) addFacet(std::string(spec.first) + ":" + std::to_string(bit));
    Statement genreList(db, "SELECT name FROM genre_options ORDER BY name");
    size_t genreCount = 0;
    while (genreList.row()) { addFacet("genre:" + lower(genreList.text(0))); ++genreCount; }
    if (genreCount > maxGenres) throw std::runtime_error("Archive genre budget exceeded");
    Statement titles(db, "SELECT id,kind,name,year,rating,votes,score FROM titles WHERE adult=0 ORDER BY id");
    Statement insert(db, "INSERT INTO records VALUES(?,?,?,?,?,?,?,?)");
    Statement genres(db, "SELECT id,genre FROM genres ORDER BY id,genre");
    bool hasGenre = genres.row();
    Statement aliases(db, "SELECT id,name FROM aliases ORDER BY id,name");
    bool hasAlias = aliases.row();
    Statement search(db, "INSERT OR IGNORE INTO title_search VALUES(?,?)");
    Statement pair(db, "INSERT INTO search_pairs VALUES(?,?)");
    // Views point into reusable normalized names, keeping even alias-heavy
    // titles bounded without allocating a tree node for every short gram.
    std::vector<std::string> names;
    std::vector<std::string_view> terms;
    uint32_t key = 0;
    buildLog("derive", count, "facets+search-pairs", started);
    exec(db, "BEGIN");
    try {
        while (titles.row()) {
            work.checkpoint();
            const auto year = titles.number(3), votes = titles.number(5);
            const auto rating = titles.real(4);
            if (year < 0 || year > 65535 || votes < 0 || uint64_t(votes) > UINT32_MAX ||
                !std::isfinite(rating) || rating < 0 || rating > 10 || std::abs(rating * 10 - std::round(rating * 10)) > 1e-6)
                throw std::runtime_error("Invalid Archive numeric value");
            insert.number(1, key);
            for (int i = 0; i < 3; ++i) insert.text(i + 2, titles.text(i));
            insert.number(5, year); insert.real(6, rating); insert.number(7, votes); insert.real(8, titles.real(6)); insert.run();
            setBit(facets.at("type:" + titles.text(1)), key);
            const std::array<std::pair<const char*, uint64_t>, 3> values{{
                {"year", uint64_t(year)}, {"rating", uint64_t(std::llround(rating * 10))}, {"votes", uint64_t(votes)}}};
            for (const auto& spec : values) {
                auto value = spec.second;
                for (unsigned bit = 0; value; ++bit, value >>= 1)
                    if (value & 1) setBit(facets.at(std::string(spec.first) + ":" + std::to_string(bit)), key);
            }
            const auto id = titles.text(0);
            while (hasGenre && genres.text(0) < id) { work.checkpoint(); hasGenre = genres.row(); }
            while (hasGenre && genres.text(0) == id) {
                setBit(facets.at("genre:" + lower(genres.text(1))), key);
                hasGenre = genres.row();
            }
            names.clear(); terms.clear();
            size_t titleBytes = 0;
            auto addName = [&](const std::string& name) {
                if (name.size() > 4096) throw std::runtime_error("Archive name budget exceeded");
                titleBytes += name.size() + 8;
                if (titleBytes > 256 * 1024) throw std::runtime_error("Archive title names budget exceeded");
                names.push_back(lower(name));
                search.number(1, key); search.text(2, names.back()); search.run();
            };
            addName(titles.text(2));
            while (hasAlias && aliases.text(0) < id) { work.checkpoint(); hasAlias = aliases.row(); }
            while (hasAlias && aliases.text(0) == id) {
                addName(aliases.text(1)); hasAlias = aliases.row();
            }
            // Names are now stable: byte substrings preserve the existing
            // ASCII lowercase / UTF-8 byte matching semantics exactly.
            for (const auto& name : names) {
                work.checkpoint();
                for (size_t width = 1; width <= 3; ++width)
                    for (size_t i = 0; i + width <= name.size(); ++i)
                        terms.emplace_back(name.data() + i, width);
                std::sort(terms.begin(), terms.end());
                terms.erase(std::unique(terms.begin(), terms.end()), terms.end());
                if (terms.size() > 65536) throw std::runtime_error("Archive title search budget exceeded");
            }
            for (const auto term : terms) { pair.text(1, term); pair.number(2, key); pair.run(); }
            if (++key % 2000 == 0) {
                exec(db, "COMMIT"); if (progress) progress(key); exec(db, "BEGIN");
            }
        }
        exec(db, "COMMIT");
        if (progress) progress(key);
    } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    work.measured("derived-records-facets-search-pairs", started);
    auto phaseStarted = Clock::now();
    Statement facet(db, "INSERT INTO facets VALUES(?,?)");
    exec(db, "BEGIN");
    try {
        for (const auto& item : facets) {
            work.checkpoint(); facet.text(1, item.first); blob(facet, 2, encode(item.second, work.yield)); facet.run();
        }
        exec(db, "COMMIT");
    } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    facets.clear();
    work.measured("facets-write", phaseStarted);
    buildLog("derive", count, "facets-complete", started);
    // Build all order arrays once, disk-backed SQLite sort during staging only.
    Statement saveSort(db, "INSERT INTO sort_orders VALUES(?,?,?)");
    for (auto name : {"year", "rating", "votes", "name"}) {
        phaseStarted = Clock::now();
        const auto column = std::string(name) == "rating" ? "score" : std::string(name) == "name" ? "lower(name)" : name;
        Statement sorted(db, "SELECT title_key," + std::string(column) + " FROM records ORDER BY " + column + ",id");
        std::vector<uint32_t> keys, ends;
        keys.reserve(count);
        std::string last;
        double lastNumber = 0;
        const bool textOrder = std::string(name) == "name";
        while (sorted.row()) {
            if (!(keys.size() % 1024)) work.checkpoint();
            auto value = sorted.text(1); // SQLite's canonical value, equality only
            if (!keys.empty() && (textOrder ? value != last : sorted.real(1) != lastNumber)) ends.push_back(keys.size());
            keys.push_back(sorted.number(0)); last = std::move(value); lastNumber = sorted.real(1);
        }
        ends.push_back(keys.size());
        if (keys.size() != count) throw std::runtime_error("Invalid Archive sort count");
        Bits seen((count + 63) / 64);
        for (auto key : keys) {
            if (!(key % 1024)) work.checkpoint();
            if (key >= count || hasBit(seen, key)) throw std::runtime_error("Invalid Archive sort reference");
            setBit(seen, key);
        }
        saveSort.text(1, name); blob(saveSort, 2, encode(keys, work.yield)); blob(saveSort, 3, encode(ends, work.yield)); saveSort.run();
        buildLog("derive", count, name, started);
        work.measured(std::string("sort-") + name, phaseStarted);
    }
    buildLog("derive", count, "search-compress", started);
    // Pack searchable names into bounded blobs, indexed by key/256. This is
    // staging-only sequential work; strings remain on disk during browsing.
    phaseStarted = Clock::now();
    {
        Statement names(db, "SELECT title_key,name FROM title_search ORDER BY title_key,name");
        Statement save(db, "INSERT INTO search_text VALUES(?,?,?)");
        std::vector<unsigned char> data;
        size_t bucket = 0, chunk = 0, titleBytes = 0, transactionBytes = 0;
        exec(db, "BEGIN");
        try {
        uint32_t previousKey = UINT32_MAX;
        auto flush = [&] {
            if (data.empty()) return;
            save.number(1, bucket); save.number(2, chunk++); blob(save, 3, data); save.run();
            transactionBytes += data.size(); data.clear();
            if (transactionBytes >= 4 * 1024 * 1024) {
                exec(db, "COMMIT"); exec(db, "BEGIN"); transactionBytes = 0;
            }
        };
        auto number = [&](uint32_t value) {
            for (unsigned b = 0; b < 4; ++b) data.push_back(value >> (8 * b));
        };
        while (names.row()) {
            work.checkpoint();
            const auto key = static_cast<uint32_t>(names.number(0));
            const auto name = names.text(1);
            if (key != previousKey) { titleBytes = 0; previousKey = key; }
            titleBytes += name.size() + 8;
            if (titleBytes > 256 * 1024) throw std::runtime_error("Archive title names budget exceeded");
            if (key / 256 != bucket) { flush(); bucket = key / 256; chunk = 0; }
            if (data.size() + name.size() + 8 > 1024 * 1024) flush();
            number(key); number(name.size()); data.insert(data.end(), name.begin(), name.end());
        }
        flush(); exec(db, "COMMIT");
        } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    }
    buildLog("derive", count, "search-text", started);
    work.measured("search-text", phaseStarted);
    phaseStarted = Clock::now();
    // Compress each sorted posting into fixed-size little-endian chunks.
    Statement pairs(db, "SELECT term,title_key FROM search_pairs ORDER BY term,title_key");
    Statement savePosting(db, "INSERT INTO search_postings VALUES(?,?,?)");
    Statement saveTerm(db, "INSERT INTO search_terms VALUES(?,?)");
    std::string term;
    std::vector<uint32_t> keys;
    size_t chunk = 0, n = 0, written = 0, committed = 0;
    auto flush = [&] {
        if (keys.empty()) return;
        savePosting.text(1, term); savePosting.number(2, chunk++); blob(savePosting, 3, encode(keys, work.yield)); savePosting.run(); keys.clear();
    };
    auto finish = [&] {
        if (!n) return;
        flush(); saveTerm.text(1, term); saveTerm.number(2, n); saveTerm.run();
    };
    exec(db, "BEGIN");
    try {
        while (pairs.row()) {
            if (!(written++ % 4096)) work.checkpoint();
            auto next = pairs.text(0);
            if (next != term) { finish(); term = std::move(next); chunk = n = 0; }
            keys.push_back(pairs.number(1)); ++n;
            if (keys.size() == postingChunk) flush();
            if (written - committed >= 262144) {
                // Bound cancellation rollback I/O to ~1 MiB of postings.
                flush(); exec(db, "COMMIT"); exec(db, "BEGIN"); committed = written;
            }
        }
        finish(); exec(db, "COMMIT");
    } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    work.measured("postings", phaseStarted);
    phaseStarted = Clock::now();
    buildLog("validate", count, "all", started);
    // Validate every reference and array before marking complete or publishing.
    Statement integrity(db, "PRAGMA quick_check");
    if (!integrity.row() || integrity.text(0) != "ok" || key != count) throw std::runtime_error("Archive validation failed");
    Statement searchCount(db, "SELECT count(*),min(title_key),max(title_key) FROM title_search");
    if (!searchCount.row() || searchCount.number(0) < int64_t(count) || searchCount.number(1) < 0 ||
        searchCount.number(2) >= int64_t(count)) throw std::runtime_error("Invalid Archive search references");
    Statement postingCounts(db, "SELECT coalesce(sum(count),0) FROM search_terms");
    if (!postingCounts.row() || postingCounts.number(0) != int64_t(written)) throw std::runtime_error("Invalid Archive posting counts");
    setting(db, "browsable", count);
    setting(db, "derived_ms", static_cast<int64_t>(elapsed(started)));
    work.measured("validation", phaseStarted);
}
} // namespace

bool buildImdbIndex(const std::string& path, const IndexCancel& cancel, const DatasetDownload& download,
        const std::function<void(size_t)>& progress, const IndexYield& yield, const BuildTiming& timing, const BuildOptions& options) {
    if (options.cacheKiB < 4096 || options.cacheKiB > 16384 ||
        options.importBatch < 2000 || options.importBatch > 20000)
        throw std::invalid_argument("Invalid Archive staging budget");
    BuildWork work{cancel, yield, timing};
    if (cancel && cancel->load()) return false;
    if (yield) yield();
    if (cancel && cancel->load()) return false;
    const auto buildStarted = Clock::now();
    const auto staging = path + ".building";
    // Old staged schemas cannot resume into a new index format.
    if (cancel && cancel->load()) return false;
    if (std::ifstream(staging).good()) {
        // A read/write probe lets SQLite recover a hot journal after process
        // termination before deciding whether this staging schema can resume.
        Database old(staging, SQLITE_OPEN_READWRITE);
        Statement schema(old.db, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='settings'");
        const bool hasSettings = schema.row() && schema.number(0);
        sqlite3_reset(schema.stmt);
        if (!hasSettings || setting(old.db, "version") != 2) {
            sqlite3_finalize(schema.stmt); schema.stmt = nullptr;
            sqlite3_close(old.db); old.db = nullptr;
            std::remove(staging.c_str()); std::remove((staging + "-journal").c_str());
        }
    }
    try {
    {
        Database connection(staging, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        auto db = connection.db;
        sqlite3_progress_handler(db, 1000, BuildWork::sqliteProgress, &work);
        const auto tuning = "PRAGMA cache_size=-" + std::to_string(options.cacheKiB) +
            "; PRAGMA synchronous=" + (options.normalSync ? std::string("NORMAL") : std::string("FULL")) +
            "; PRAGMA temp_store=FILE; PRAGMA journal_mode=DELETE;"
            "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value INTEGER);";
        exec(db, tuning.c_str());
        if (!setting(db, "version")) setting(db, "version", 2);
        if (!setting(db, "derived_ready") && !setting(db, "complete")) exec(db,
            "CREATE TABLE IF NOT EXISTS ratings(id TEXT PRIMARY KEY,rating REAL,votes INTEGER) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS titles(id TEXT PRIMARY KEY,kind TEXT,name TEXT,year INTEGER,"
            "rating REAL,votes INTEGER,adult INTEGER,score REAL) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS aliases(id TEXT,name TEXT,PRIMARY KEY(id,name)) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS genres(id TEXT,genre TEXT,PRIMARY KEY(id,genre)) WITHOUT ROWID;");
        const std::array<const char*, 3> datasets{"title.ratings.tsv.gz", "title.basics.tsv.gz", "title.akas.tsv.gz"};
        for (int phase = static_cast<int>(setting(db, "phase")); phase < 3; ++phase) {
            buildLog("import", setting(db, "indexed"), datasets[phase], buildStarted);
            if (cancel->load()) return false;
            const auto downloadStarted = Clock::now();
            const auto gzipPath = path + "." + datasets[phase];
            if (!std::ifstream(gzipPath, std::ios::binary).good()) {
                download(datasets[phase], gzipPath, cancel);
                if (cancel->load()) return false;
            }
            work.measured(std::string("download-") + datasets[phase], downloadStarted);
            const auto importStarted = Clock::now();
            Gzip input(gzipPath);
            auto offset = setting(db, "offset");
            // gzseek can spend a long time inflating a large prefix without a
            // cancellation point. Skip in small blocks so playback stays first.
            std::array<char, 64 * 1024> prefix;
            for (int64_t skipped = 0; skipped < offset;) {
                work.checkpoint();
                if (cancel->load()) return false;
                int bytes = gzread(input.file, prefix.data(), std::min<int64_t>(prefix.size(), offset - skipped));
                if (bytes <= 0) throw InvalidDataset("Cannot resume IMDb dataset");
                skipped += bytes;
            }
            std::string line;
            if (!offset && (!input.line(line) || line.rfind(phase == 2 ? "titleId\t" : "tconst\t", 0) != 0))
                throw InvalidDataset("Invalid IMDb dataset header");
            Statement ratingInsert(db, "INSERT OR REPLACE INTO ratings VALUES(?,?,?)");
            Statement ratingFind(db, "SELECT rating,votes FROM ratings WHERE id=?");
            Statement titleInsert(db, "INSERT OR IGNORE INTO titles VALUES(?,?,?,?,?,?,?,0)");
            Statement aliasInsert(db, "INSERT OR IGNORE INTO aliases VALUES(?,lower(?))");
            Statement genreInsert(db, "INSERT OR IGNORE INTO genres VALUES(?,?)");
            Statement italian(db, "UPDATE titles SET name=? WHERE id=? AND NOT EXISTS(SELECT 1 FROM settings WHERE key='it:'||?)");
            Statement italianSeen(db, "INSERT OR IGNORE INTO settings VALUES('it:'||?,1)");
            Statement exists(db, "SELECT 1 FROM titles WHERE id=?");
            size_t batch = 0;
            size_t indexed = static_cast<size_t>(setting(db, "indexed"));
            exec(db, "BEGIN");
            try {
                auto checkpoint = [&] {
                    // A checkpoint must finish even when cancellation arrived
                    // in this batch. Restore the handler for subsequent work.
                    sqlite3_progress_handler(db, 0, nullptr, nullptr);
                    setting(db, "offset", gztell(input.file));
                    setting(db, "indexed", indexed);
                    exec(db, "COMMIT");
                    sqlite3_progress_handler(db, 1000, BuildWork::sqliteProgress, &work);
                    if (progress) progress(indexed);
                };
                while (input.line(line, [&] { work.checkpoint(); })) {
                    if (!(batch % 64)) work.checkpoint();
                    auto fields = split(line);
                    if (phase == 0 && fields.size() >= 3) {
                        auto votes = integer(fields[2]);
                        if (votes >= 100) {
                            ratingInsert.text(1, fields[0]);
                            ratingInsert.real(2, std::strtod(std::string(fields[1]).c_str(), nullptr));
                            ratingInsert.number(3, votes); ratingInsert.run();
                        }
                    } else if (phase == 1 && fields.size() >= 9 && fields[1] != "tvEpisode" && fields[1] != "videoGame") {
                        ratingFind.text(1, fields[0]);
                        if (ratingFind.row()) {
                            const auto kind = fields[1] == "tvSeries" || fields[1] == "tvMiniSeries" ? "series" : "movie";
                            titleInsert.text(1, fields[0]); titleInsert.text(2, kind); titleInsert.text(3, fields[2]);
                            titleInsert.number(4, integer(fields[5])); titleInsert.real(5, ratingFind.real(0));
                            titleInsert.number(6, ratingFind.number(1)); titleInsert.number(7, integer(fields[4])); titleInsert.run();
                            if (sqlite3_changes(db)) ++indexed;
                            for (int i : {2, 3}) { aliasInsert.text(1, fields[0]); aliasInsert.text(2, fields[i]); aliasInsert.run(); }
                            if (fields[8] != "\\N") {
                                auto genreText = std::string(fields[8]);
                                for (auto genre : split(genreText, ',')) {
                                    genreInsert.text(1, fields[0]); genreInsert.text(2, genre); genreInsert.run();
                                }
                            }
                        }
                        sqlite3_reset(ratingFind.stmt); sqlite3_clear_bindings(ratingFind.stmt);
                    } else if (phase == 2 && fields.size() >= 5 &&
                        (fields[3] == "IT" || fields[3] == "US" || fields[3] == "GB" || fields[3] == "XWW" ||
                         fields[3] == "CA" || fields[3] == "AU" || fields[4] == "en")) {
                        exists.text(1, fields[0]);
                        if (exists.row()) {
                            aliasInsert.text(1, fields[0]); aliasInsert.text(2, fields[2]); aliasInsert.run();
                            if (fields[3] == "IT") {
                                italian.text(1, fields[2]); italian.text(2, fields[0]); italian.text(3, fields[0]); italian.run();
                                italianSeen.text(1, fields[0]); italianSeen.run();
                            }
                        }
                        sqlite3_reset(exists.stmt); sqlite3_clear_bindings(exists.stmt);
                    }
                    if (++batch >= options.importBatch || cancel->load()) {
                        checkpoint();
                        if (cancel->load()) return false;
                        exec(db, "BEGIN"); batch = 0;
                    }
                }
                setting(db, "phase", phase + 1); setting(db, "offset", 0); setting(db, "indexed", indexed);
                exec(db, "COMMIT");
                if (progress) progress(indexed);
            } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
            work.measured(std::string("import-") + datasets[phase], importStarted);
        }
        if (cancel->load()) return false;
        // Derived indexes are restartable, never exposed during their build.
        if (!setting(db, "complete")) {
            if (!setting(db, "derived_ready")) {
                exec(db, "UPDATE titles SET score=(votes*rating+25000*coalesce((SELECT avg(rating) FROM titles WHERE votes>=1000),6.5))/(votes+25000);");
                buildDerived(db, work, progress);
                work.checkpoint();
                setting(db, "derived_ready", 1);
            }
            exec(db, "DROP TABLE IF EXISTS search_pairs; DROP TABLE IF EXISTS ratings;"
                "DROP TABLE IF EXISTS aliases; DROP TABLE IF EXISTS genres; DROP TABLE IF EXISTS titles; DROP TABLE IF EXISTS title_search;"
                "DELETE FROM settings WHERE key LIKE 'it:%';");
            // Compact freed staging pages before publication, cancellable by
            // SQLite's handler. Temporary peak disk is documented.
            buildLog("compact", setting(db, "browsable"), "all", buildStarted);
            const auto compactStarted = Clock::now();
            exec(db, "VACUUM");
            work.measured("vacuum", compactStarted);
            work.checkpoint();
            setting(db, "refreshed", timestamp()); setting(db, "complete", 1);
        }
        const auto finalValidationStarted = Clock::now();
        Statement finalCheck(db, "PRAGMA quick_check");
        if (!finalCheck.row() || finalCheck.text(0) != "ok")
            throw std::runtime_error("Archive final validation failed");
        work.measured("validation-final", finalValidationStarted);
    }
    // Close every statement/connection before swapping; failed downloads/builds
    // leave the previously published database available for browsing.
    if (cancel->load()) return false;
    buildLog("publish", 0, "v2", buildStarted);
    if (std::rename(staging.c_str(), path.c_str()) != 0) throw std::runtime_error("Cannot publish IMDb index");
    for (auto name : {"title.ratings.tsv.gz", "title.basics.tsv.gz", "title.akas.tsv.gz"})
        std::remove((path + "." + name).c_str());
    work.measured("total", buildStarted);
    return true;
    } catch (const InvalidDataset&) {
        // A malformed/truncated provider response must not poison every retry.
        // Discard this build's offsets together with its gzip files; keep the
        // published index, whose connection/file was never modified.
        std::remove(staging.c_str());
        std::remove((staging + "-journal").c_str());
        for (auto name : {"title.ratings.tsv.gz", "title.basics.tsv.gz", "title.akas.tsv.gz"})
            std::remove((path + "." + name).c_str());
        throw;
    } catch (...) {
        if (cancel && cancel->load()) return false;
        throw;
    }
}
} // namespace stremio::archive
