#include "api/stremio/imdb_index.hpp"
#include <sqlite3.h>
#include <zlib.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string_view>

namespace stremio::archive {
namespace {
struct InvalidDataset : std::runtime_error { using std::runtime_error::runtime_error; };
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
        vfs = "unix-none";
#endif
        if (sqlite3_open_v2(path.c_str(), &db, flags, vfs) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            throw std::runtime_error("Cannot open IMDb index");
        }
        sqlite3_busy_timeout(db, 3000);
    }
    ~Database() { if (db) sqlite3_close(db); }
};
void exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot update IMDb index");
}
struct Statement {
    sqlite3_stmt* stmt = nullptr;
    Statement(sqlite3* db, const std::string& sql) {
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
    bool line(std::string& out) {
        out.clear();
        std::array<char, 4096> buffer;
        while (gzgets(file, buffer.data(), buffer.size())) {
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

ImdbIndex::ImdbIndex(const std::string& path) {
    Database connection(path, SQLITE_OPEN_READONLY);
    if (setting(connection.db, "version") != 1 || !setting(connection.db, "complete"))
        throw std::runtime_error("Incomplete IMDb index");
    Statement total(connection.db, "SELECT count(*) FROM titles WHERE adult=0");
    if (!total.row() || !(count = total.number(0))) throw std::runtime_error("Empty IMDb index");
    refreshed = setting(connection.db, "refreshed");
    Statement genres(connection.db, "SELECT DISTINCT genre FROM genres ORDER BY genre");
    while (genres.row()) genreOptions.push_back(genres.text(0));
    db = connection.db; connection.db = nullptr;
}
ImdbIndex::~ImdbIndex() { sqlite3_close(db); }

IndexResult ImdbIndex::query(const Filter& filter, size_t offset, size_t limit, bool random) {
    IndexResult result;
    result.indexed = count; result.refreshed = refreshed;
    if (!limit && !random) return result;
    result.genres = genreOptions;
    // IMDb does not supply country, streaming availability, views or synopses.
    // Preserve their meaning instead of presenting votes as public views.
    if (!filter.country.empty() || !filter.service.empty() || !filter.addon.empty() || filter.minViews || filter.other == 2)
        return result;
    // Add only selected predicates so SQLite can use its sort/type indices.
    std::string where = " WHERE t.adult=0";
    if (!filter.type.empty()) where += " AND t.kind=?1";
    if (!filter.search.empty()) where += " AND (instr(lower(t.name),?2)>0 OR EXISTS(SELECT 1 FROM aliases a WHERE a.id=t.id AND instr(a.name,?2)>0))";
    if (!filter.genre.empty()) where += " AND EXISTS(SELECT 1 FROM genres g WHERE g.id=t.id AND lower(g.genre)=?3)";
    if (filter.yearFrom) where += " AND t.year>=?4";
    if (filter.yearTo) where += " AND t.year>0 AND t.year<=?5";
    if (filter.minRating) where += " AND t.rating>=?6";
    if (filter.minVotes) where += " AND t.votes>=?7";
    auto bind = [&](Statement& stmt) {
        if (!filter.type.empty()) stmt.text(1, filter.type);
        if (!filter.search.empty()) stmt.text(2, lower(filter.search));
        if (!filter.genre.empty()) stmt.text(3, lower(filter.genre));
        if (filter.yearFrom) stmt.number(4, filter.yearFrom);
        if (filter.yearTo) stmt.number(5, filter.yearTo);
        if (filter.minRating) stmt.real(6, filter.minRating);
        if (filter.minVotes) stmt.number(7, filter.minVotes);
    };
    if (where == " WHERE t.adult=0") result.total = count;
    else {
        Statement total(db, "SELECT count(*) FROM titles t" + where); bind(total);
        if (total.row()) result.total = total.number(0);
    }
    if (!result.total) return result;
    // Count + one uniformly sampled offset avoids random-sort of the whole DB.
    if (random) {
        static std::mt19937 engine(static_cast<uint32_t>(timestamp()));
        offset = std::uniform_int_distribution<size_t>(0, result.total - 1)(engine);
        limit = 1;
    }
    std::string order;
    switch (filter.sort) {
        case Sort::Name: order = "lower(t.name)"; break;
        case Sort::Rating: order = "t.score"; break;
        case Sort::Votes: order = "t.votes"; break;
        case Sort::Added: case Sort::Updated: order = "t.id"; break;
        default: order = "t.year"; break;
    }
    Statement rows(db, "SELECT t.id,t.kind,t.name,t.year,t.rating,t.votes FROM titles t" + where +
        " ORDER BY " + order + (filter.descending ? " DESC" : " ASC") + ",t.id ASC LIMIT ?8 OFFSET ?9");
    bind(rows); rows.number(8, limit); rows.number(9, offset);
    while (rows.row()) {
        Record record;
        record.meta = {{"id", rows.text(0)}, {"type", rows.text(1)}, {"name", rows.text(2)},
            {"year", std::to_string(rows.number(3))}, {"releaseInfo", std::to_string(rows.number(3))},
            {"imdbRating", rows.real(4)}, {"votes", rows.number(5)},
            {"poster", "https://images.metahub.space/poster/small/" + rows.text(0) + "/img"}};
        record.added = record.updated = refreshed;
        result.records.push_back(std::move(record));
    }
    return result;
}

bool buildImdbIndex(const std::string& path, const IndexCancel& cancel, const DatasetDownload& download,
        const std::function<void(size_t)>& progress) {
    const auto staging = path + ".building";
    try {
    {
        Database connection(staging, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
        auto db = connection.db;
        sqlite3_progress_handler(db, 1000, [](void* value) {
            return static_cast<std::atomic_bool*>(value)->load() ? 1 : 0;
        }, cancel.get());
        exec(db, "PRAGMA cache_size=-4096; PRAGMA temp_store=FILE; PRAGMA journal_mode=DELETE;"
            "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value INTEGER);"
            "CREATE TABLE IF NOT EXISTS ratings(id TEXT PRIMARY KEY,rating REAL,votes INTEGER) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS titles(id TEXT PRIMARY KEY,kind TEXT,name TEXT,year INTEGER,"
            "rating REAL,votes INTEGER,adult INTEGER,score REAL) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS aliases(id TEXT,name TEXT,PRIMARY KEY(id,name)) WITHOUT ROWID;"
            "CREATE TABLE IF NOT EXISTS genres(id TEXT,genre TEXT,PRIMARY KEY(id,genre)) WITHOUT ROWID;");
        if (!setting(db, "version")) setting(db, "version", 1);
        const std::array<const char*, 3> datasets{"title.ratings.tsv.gz", "title.basics.tsv.gz", "title.akas.tsv.gz"};
        for (int phase = static_cast<int>(setting(db, "phase")); phase < 3; ++phase) {
            if (cancel->load()) return false;
            const auto gzipPath = path + "." + datasets[phase];
            if (!std::ifstream(gzipPath, std::ios::binary).good()) {
                download(datasets[phase], gzipPath, cancel);
                if (cancel->load()) return false;
            }
            Gzip input(gzipPath);
            auto offset = setting(db, "offset");
            // gzseek can spend a long time inflating a large prefix without a
            // cancellation point. Skip in small blocks so playback stays first.
            std::array<char, 64 * 1024> prefix;
            for (int64_t skipped = 0; skipped < offset;) {
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
                    sqlite3_progress_handler(db, 1000, [](void* value) {
                        return static_cast<std::atomic_bool*>(value)->load() ? 1 : 0;
                    }, cancel.get());
                    if (progress) progress(indexed);
                };
                while (input.line(line)) {
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
                    if (++batch >= 2000 || cancel->load()) {
                        checkpoint();
                        if (cancel->load()) return false;
                        exec(db, "BEGIN"); batch = 0;
                    }
                }
                setting(db, "phase", phase + 1); setting(db, "offset", 0); setting(db, "indexed", indexed);
                exec(db, "COMMIT");
                if (progress) progress(indexed);
            } catch (...) { sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
        }
        if (cancel->load()) return false;
        // Keep the TV box's Bayesian rating order; the displayed rating stays raw.
        if (!setting(db, "complete")) {
        exec(db, "BEGIN");
        exec(db, "UPDATE titles SET score=(votes*rating+25000*coalesce((SELECT avg(rating) FROM titles WHERE votes>=1000),6.5))/(votes+25000);"
            "CREATE INDEX IF NOT EXISTS title_year ON titles(year,id);"
            "CREATE INDEX IF NOT EXISTS title_rating ON titles(score,id);"
            "CREATE INDEX IF NOT EXISTS title_votes ON titles(votes,id);"
            "CREATE INDEX IF NOT EXISTS title_name ON titles(lower(name),id);"
            "CREATE INDEX IF NOT EXISTS title_kind_year ON titles(kind,year,id) WHERE adult=0;"
            "CREATE INDEX IF NOT EXISTS title_kind_rating ON titles(kind,score,id) WHERE adult=0;"
            "CREATE INDEX IF NOT EXISTS title_kind_votes ON titles(kind,votes,id) WHERE adult=0;"
            "CREATE INDEX IF NOT EXISTS title_kind_name ON titles(kind,lower(name),id) WHERE adult=0;"
            "CREATE INDEX IF NOT EXISTS genre_lookup ON genres(genre,id);"
            "DROP TABLE IF EXISTS ratings; DELETE FROM settings WHERE key LIKE 'it:%';");
        if (!setting(db, "indexed")) throw std::runtime_error("Empty IMDb dataset");
        setting(db, "complete", 1); setting(db, "refreshed", timestamp());
        exec(db, "COMMIT");
        }
    }
    // Close every statement/connection before swapping; failed downloads/builds
    // leave the previously published database available for browsing.
    if (cancel->load()) return false;
    if (std::rename(staging.c_str(), path.c_str()) != 0) throw std::runtime_error("Cannot publish IMDb index");
    for (auto name : {"title.ratings.tsv.gz", "title.basics.tsv.gz", "title.akas.tsv.gz"})
        std::remove((path + "." + name).c_str());
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
    }
}
} // namespace stremio::archive
