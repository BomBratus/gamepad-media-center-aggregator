#include "api/stremio/imdb_index.hpp"
#include "api/stremio/types.hpp"
#include <zlib.h>
#include <sqlite3.h>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <iostream>
#include <set>
#include <unistd.h>
#include <sys/stat.h>
using namespace stremio::archive;

static int unsupportedLstat(const char*, struct stat*) { errno = ENOSYS; return -1; }

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("gmca-imdb-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto path = (directory / "index.sqlite").string();
    // Model OpenOrbis musl: lstat -> fstatat is unimplemented (ENOSYS).
    // unix-none removes locking but still resolves every parent via lstat.
    auto unixVfs = sqlite3_vfs_find("unix-none");
    assert(unixVfs && unixVfs->xSetSystemCall(unixVfs, "lstat",
        reinterpret_cast<sqlite3_syscall_ptr>(unsupportedLstat)) == SQLITE_OK);
    sqlite3* probe = nullptr;
    auto probePath = path + ".probe";
    assert(sqlite3_open_v2(probePath.c_str(), &probe, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        "unix-none") == SQLITE_CANTOPEN);
    sqlite3_close(probe);
    assert(!std::filesystem::exists(probePath));
    auto cancel = std::make_shared<std::atomic_bool>(false);
    int downloads = 0;
    auto fixtures = [&](const std::string& name, const std::string& output, const IndexCancel&) {
        ++downloads;
        std::string body;
        if (name == "title.ratings.tsv.gz") {
            body = "tconst\taverageRating\tnumVotes\n";
            for (int i = 1; i <= 2505; ++i) body += "tt" + std::to_string(i) + "\t8.0\t1000\n";
            body += "ttlow\t9.0\t99\n";
        } else if (name == "title.basics.tsv.gz") {
            body = "tconst\ttitleType\tprimaryTitle\toriginalTitle\tisAdult\tstartYear\tendYear\truntimeMinutes\tgenres\n";
            for (int i = 1; i <= 2505; ++i)
                body += "tt" + std::to_string(i) + "\t" + (i % 2 ? "movie" : "tvSeries") + "\tTitle " + std::to_string(i) +
                    "\tOriginal " + std::to_string(i) + "\t" + (i == 3 ? "1" : "0") + "\t2020\t\\N\t90\tDrama,Comedy\n";
            body += "ttlow\tmovie\tLow\tLow\t0\t2020\t\\N\t90\tDrama\n";
            body += "ttepisode\ttvEpisode\tEpisode\tEpisode\t0\t2020\t\\N\t20\tDrama\n";
        } else {
            body = "titleId\tordering\ttitle\tregion\tlanguage\ttypes\tattributes\tisOriginalTitle\n"
                "tt1\t1\tItaliano\tIT\tit\t\\N\t\\N\t0\n"
                "tt1\t2\tSecond Italian\tIT\tit\t\\N\t\\N\t0\n"
                "tt1\t3\tEnglish Alias\tUS\ten\t\\N\t\\N\t0\n"
                "tt1\t4\tIgnore This\tFR\tfr\t\\N\t\\N\t0\n";
        }
        auto gzip = gzopen(output.c_str(), "wb"); assert(gzip);
        assert(gzwrite(gzip, body.data(), body.size()) == static_cast<int>(body.size()));
        assert(gzclose(gzip) == Z_OK);
    };
    bool paused = false;
    assert(!buildImdbIndex(path, cancel, fixtures, [&](size_t count) {
        if (count >= 2000 && !paused) { paused = true; cancel->store(true); }
    }));
    assert(downloads == 2 && !std::filesystem::exists(path));
    cancel->store(false);
    assert(buildImdbIndex(path, cancel, fixtures));
    assert(downloads == 3); // completed datasets reused after interruption
    auto old = std::make_shared<ImdbIndex>(path);
    auto all = old->query({}, 0, 60, false);
    assert(all.indexed == 2504 && all.total == 2504 && all.records.size() == 60);
    assert(all.genres == std::vector<std::string>({"Comedy", "Drama"}));
    Filter filter;
    filter.search = "ENGLISH ALIAS";
    auto found = old->query(filter, 0, 60, false);
    assert(found.total == 1 && found.records[0].meta["name"] == "Italiano");
    auto card = stremio::parseMetaPreview(found.records[0].meta);
    assert(card.ratingKey == "movie:tt1" && card.year == 2020 && card.rating == 8.0);
    assert(card.title == "Italiano" && card.thumb.find("/tt1/") != std::string::npos);
    filter.search = "Original 1";
    assert(old->query(filter, 0, 60, false).total > 1);
    // Cached substring matches must remain complete and deduplicated across
    // aliases, pagination, sort changes, Random and changes to other filters.
    filter.search = "title";
    auto titles = old->query(filter, 0, 60, false);
    assert(titles.total == 2503); // adult title excluded; tt1 is now Italiano
    auto titlesNext = old->query(filter, 60, 60, false);
    assert(titlesNext.total == titles.total && titlesNext.records.size() == 60);
    std::set<std::string> titleIds;
    for (const auto& record : titles.records) titleIds.insert(record.meta["id"]);
    for (const auto& record : titlesNext.records) assert(!titleIds.count(record.meta["id"]));
    filter.sort = Sort::Name; filter.descending = false;
    auto named = old->query(filter, 0, 60, false);
    assert(named.total == titles.total && named.records.front().meta["name"] == "Title 10");
    filter.type = "series";
    assert(old->query(filter, 0, 60, false).total == 1252);
    filter.yearTo = 2019;
    assert(old->query(filter, 0, 60, false).total == 0);
    filter.yearTo = 2020;
    assert(old->query(filter, 0, 60, true).total == 1252);
    assert(old->query({}, 0, 60, false).total == 2504);
    assert(old->query(filter, 0, 60, false).total == 1252);
    filter = {}; filter.search = "1";
    assert(old->query(filter, 0, 60, false).total > 1); // short substring allowed
    filter.search = "%_";
    assert(old->query(filter, 0, 60, false).total == 0); // literal, not LIKE syntax
    filter.search = "ENGLISH ALIAS";
    assert(old->query(filter, 0, 60, false).total == 1);
    filter.search = "Ignore This";
    assert(old->query(filter, 0, 60, false).total == 0);
    filter = {}; filter.type = "series"; filter.genre = "drama"; filter.minRating = 7; filter.minVotes = 1000;
    filter.yearFrom = filter.yearTo = 2020; filter.sort = Sort::Votes;
    auto series = old->query(filter, 0, 60, false);
    assert(series.total == 1252);
    auto page2 = old->query(filter, 60, 60, false);
    std::set<std::string> ids;
    for (auto& record : series.records) ids.insert(record.meta["id"]);
    for (auto& record : page2.records) assert(!ids.count(record.meta["id"]));
    bool beyondFirst = false;
    for (int i = 0; i < 100; ++i) {
        auto pick = old->query(filter, 0, 60, true);
        assert(pick.records.size() == 1 && pick.total == series.total);
        if (!ids.count(pick.records[0].meta["id"])) beyondFirst = true;
    }
    assert(beyondFirst);
    filter.country = "Italy"; assert(old->query(filter, 0, 60, false).records.empty());
    filter = {}; filter.minViews = 100; assert(old->query(filter, 0, 60, false).records.empty());
    filter = {}; filter.minVotes = 1001; assert(old->query(filter, 0, 60, false).records.empty());
    assert(old->query({}, 0, 0, false).indexed == 2504);
    // A failed replacement must preserve the active index and the reader's generation.
    bool failed = false;
    try { buildImdbIndex(path, cancel, [](const auto&, const auto&, const auto&) { throw std::runtime_error("offline"); }); }
    catch (...) { failed = true; }
    assert(failed && ImdbIndex(path).query({}, 0, 0, false).indexed == 2504);
    assert(buildImdbIndex(path, cancel, fixtures));
    assert(old->query({}, 0, 60, false).total == 2504);
    failed = false;
    try {
        buildImdbIndex(path, cancel, [](const auto&, const auto& output, const auto&) {
            auto gzip = gzopen(output.c_str(), "wb");
            gzputs(gzip, "Not an IMDb dataset\n"); gzclose(gzip);
        });
    } catch (...) { failed = true; }
    assert(failed && !std::filesystem::exists(path + ".building"));
    assert(ImdbIndex(path).query({}, 0, 0, false).indexed == 2504);
    assert(buildImdbIndex(path, cancel, fixtures)); // a clean retry can recover
    // Reopening an older published generation must work without rebuilding or
    // changing the file. Pre-00.92 indexes lack the cached browsable count.
    sqlite3* legacy = nullptr;
    assert(sqlite3_open_v2(path.c_str(), &legacy, SQLITE_OPEN_READWRITE, "gmca-ps4-index") == SQLITE_OK);
    assert(sqlite3_exec(legacy, "DELETE FROM settings WHERE key='browsable'", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    auto reopened = ImdbIndex(path).query({}, 0, 60, false);
    assert(reopened.indexed == 2504 && reopened.records.size() == 60);
    assert(reopened.genres == std::vector<std::string>({"Comedy", "Drama"}));
    old.reset();
    std::filesystem::remove_all(directory);
    std::cout << "IMDb disk index tests passed\n";
}
