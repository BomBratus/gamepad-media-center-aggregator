#include "api/stremio/imdb_index.hpp"
#include "api/stremio/archive_playback_gate.hpp"
#include <future>
#include "api/stremio/types.hpp"
#include <zlib.h>
#include <sqlite3.h>
#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <unistd.h>
#include <sys/stat.h>
using namespace stremio::archive;

static int unsupportedLstat(const char*, struct stat*) { errno = ENOSYS; return -1; }

static IndexResult testPage(ImdbIndex& index, const Filter& filter, size_t offset, size_t limit, bool random,
        const IndexCancel& cancel = {}) {
    Cursor cursor;
    while (offset) {
        auto step = std::min<size_t>(offset, 60);
        cursor = index.query(filter, cursor, step, false, cancel).cursor;
        offset -= step;
    }
    return index.query(filter, cursor, limit, random, cancel);
}
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
            for (int i = 1; i <= 2505; ++i) body += "tt" + std::to_string(i) + (i == 2505 ? "\t9.2\t1000\n" : "\t8.0\t1000\n");
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
    // A process can die before even creating the settings table. Empty staging
    // must be discarded cleanly instead of poisoning every subsequent rebuild.
    { std::ofstream empty(path + ".building"); }
    assert(buildImdbIndex(path, cancel, fixtures));
    auto old = std::make_shared<ImdbIndex>(path);
    auto all = testPage(*old, {}, 0, 60, false);
    assert(all.indexed == 2504 && all.total == 2504 && all.records.size() == 60);
    assert(all.genres == std::vector<std::string>({"Comedy", "Drama"}));
    Filter filter;
    filter.search = "ENGLISH ALIAS";
    auto found = testPage(*old, filter, 0, 60, false);
    assert(found.total == 1 && found.records[0].meta["name"] == "Italiano");
    auto card = stremio::parseMetaPreview(found.records[0].meta);
    assert(card.ratingKey == "movie:tt1" && card.year == 2020 && card.rating == 8.0);
    assert(card.title == "Italiano" && card.thumb.find("/tt1/") != std::string::npos);
    filter.search = "Original 1";
    assert(testPage(*old, filter, 0, 60, false).total > 1);
    // Cached substring matches must remain complete and deduplicated across
    // aliases, pagination, sort changes, Random and changes to other filters.
    filter.search = "title";
    auto titles = testPage(*old, filter, 0, 60, false);
    assert(titles.total == 2504); // adult excluded; tt1 still matches its original alias
    auto titlesNext = testPage(*old, filter, 60, 60, false);
    assert(titlesNext.total == titles.total && titlesNext.records.size() == 60);
    std::set<std::string> titleIds;
    for (const auto& record : titles.records) titleIds.insert(record.meta["id"]);
    for (const auto& record : titlesNext.records) assert(!titleIds.count(record.meta["id"]));
    filter.sort = Sort::Name; filter.descending = false;
    auto named = testPage(*old, filter, 0, 60, false);
    assert(named.total == titles.total && named.records.front().meta["name"] == "Italiano");
    filter.type = "series";
    assert(testPage(*old, filter, 0, 60, false).total == 1252);
    filter.yearTo = 2019;
    assert(testPage(*old, filter, 0, 60, false).total == 0);
    filter.yearTo = 2020;
    assert(testPage(*old, filter, 0, 60, true).total == 1252);
    assert(testPage(*old, {}, 0, 60, false).total == 2504);
    assert(testPage(*old, filter, 0, 60, false).total == 1252);
    filter = {}; filter.search = "1";
    assert(testPage(*old, filter, 0, 60, false).total > 1); // short substring allowed
    filter.search = "%_";
    assert(testPage(*old, filter, 0, 60, false).total == 0); // literal, not LIKE syntax
    filter.search = "ENGLISH ALIAS";
    assert(testPage(*old, filter, 0, 60, false).total == 1);
    // Change each pre-existing filter on a cached match set; an identical
    // search must not preserve a count or page from the previous predicates.
    Filter base; base.search = "title";
    auto check = [&](Filter value, size_t expected) {
        auto page = testPage(*old, value, 0, 60, false);
        assert(page.total == expected && page.records.size() == std::min<size_t>(60, expected));
        for (const auto& record : page.records) {
            assert(rating(record) >= value.minRating);
            if (!value.type.empty()) assert(record.meta["type"] == value.type);
            if (value.yearFrom) assert(year(record) >= value.yearFrom);
            if (value.yearTo) assert(year(record) <= value.yearTo);
        }
        assert(testPage(*old, value, 60, 60, false).total == expected);
        auto picked = testPage(*old, value, 0, 60, true);
        assert(picked.total == expected && picked.records.size() == (expected ? 1 : 0));
    };
    check(base, 2504);
    { auto f = base; f.minRating = 9; check(f, 1); f.minRating = 8; check(f, 2504); }
    { auto f = base; f.minVotes = 1001; check(f, 0); f.minVotes = 1000; check(f, 2504); }
    { auto f = base; f.yearFrom = 2021; check(f, 0); f.yearFrom = 2020; check(f, 2504); }
    { auto f = base; f.yearTo = 2019; check(f, 0); f.yearTo = 2020; check(f, 2504); }
    { auto f = base; f.type = "movie"; check(f, 1252); f.type = "series"; check(f, 1252); }
    { auto f = base; f.genre = "missing"; check(f, 0); f.genre = "COMEDY"; check(f, 2504); }
    for (auto field : {&Filter::country, &Filter::service, &Filter::addon}) {
        auto f = base; f.*field = "unavailable"; check(f, 0);
    }
    { auto f = base; f.minViews = 1; check(f, 0); }
    { auto f = base; f.other = 1; check(f, 2504); f.other = 2; check(f, 0); }
    for (auto sort : {Sort::Release, Sort::Rating, Sort::Votes, Sort::Name, Sort::Added, Sort::Updated}) {
        for (bool descending : {false, true}) {
            auto f = base; f.sort = sort; f.descending = descending; check(f, 2504);
        }
    }
    // Rating-only queries (no text) use the same bounded sequential path.
    { Filter f; f.minRating = 9; check(f, 1); f.minRating = 8; check(f, 2504); }
    filter.search = "Ignore This";
    assert(testPage(*old, filter, 0, 60, false).total == 0);
    filter = {}; filter.type = "series"; filter.genre = "drama"; filter.minRating = 7; filter.minVotes = 1000;
    filter.yearFrom = filter.yearTo = 2020; filter.sort = Sort::Votes;
    auto series = testPage(*old, filter, 0, 60, false);
    assert(series.total == 1252);
    auto page2 = testPage(*old, filter, 60, 60, false);
    std::set<std::string> ids;
    for (auto& record : series.records) ids.insert(record.meta["id"]);
    for (auto& record : page2.records) assert(!ids.count(record.meta["id"]));
    bool beyondFirst = false;
    for (int i = 0; i < 100; ++i) {
        auto pick = testPage(*old, filter, 0, 60, true);
        assert(pick.records.size() == 1 && pick.total == series.total);
        if (!ids.count(pick.records[0].meta["id"])) beyondFirst = true;
    }
    assert(beyondFirst);
    filter.country = "Italy"; assert(testPage(*old, filter, 0, 60, false).records.empty());
    filter = {}; filter.minViews = 100; assert(testPage(*old, filter, 0, 60, false).records.empty());
    filter = {}; filter.minVotes = 1001; assert(testPage(*old, filter, 0, 60, false).records.empty());
    assert(testPage(*old, {}, 0, 0, false).indexed == 2504);

    // Superseded interactive scans stop cooperatively through SQLite's progress
    // handler, and the handler/cache state is clean for the next request.
    auto queryCancel = std::make_shared<std::atomic_bool>(true);
    bool interrupted = false;
    try {
        Filter cancelledFilter;
        cancelledFilter.search = "definitely-not-present";
        testPage(*old, cancelledFilter, 0, 60, false, queryCancel);
    } catch (...) { interrupted = true; }
    assert(interrupted);
    Filter afterCancel;
    afterCancel.search = "ENGLISH ALIAS";
    assert(testPage(*old, afterCancel, 0, 60, false).total == 1);

    // v2 work accounting: predicates never scan/sort SQL rows. Cursor pages
    // and sort-only resets reuse the same match object; Random does too.
    Filter engineFilter; engineFilter.type = "movie"; engineFilter.genre = "Drama";
    engineFilter.yearFrom = 2020; engineFilter.yearTo = 2026;
    engineFilter.minRating = 8; engineFilter.minVotes = 1000;
    auto firstPage = old->query(engineFilter, {}, 60, false);
    assert(firstPage.metrics.sqlFullScanSteps == 0 && firstPage.metrics.sqlSorts == 0);
    assert(firstPage.metrics.metadataRows == 60);
    auto secondPage = old->query(engineFilter, firstPage.cursor, 60, false);
    assert(secondPage.cursor.matches == firstPage.cursor.matches);
    assert(secondPage.cursor.position > firstPage.cursor.position);
    assert(secondPage.metrics.reusedMatches && secondPage.metrics.facetWords == 0);
    assert(secondPage.metrics.sqlFullScanSteps == 0 && secondPage.metrics.sqlSorts == 0);
    engineFilter.sort = Sort::Name;
    auto sortChanged = old->query(engineFilter, {}, 60, false);
    assert(sortChanged.cursor.matches == firstPage.cursor.matches && sortChanged.metrics.reusedMatches);
    auto rejectCursor = [&](ImdbIndex& index, Filter f, Cursor cursor) {
        bool rejected = false;
        try { index.query(f, cursor, 60, false); } catch (...) { rejected = true; }
        assert(rejected);
    };
    rejectCursor(*old, engineFilter, firstPage.cursor); // changed sort
    engineFilter.sort = Sort::Release; engineFilter.minRating = 9;
    rejectCursor(*old, engineFilter, firstPage.cursor); // changed predicates
    engineFilter.minRating = 8; engineFilter.descending = false;
    rejectCursor(*old, engineFilter, firstPage.cursor); // changed direction
    // All key visits over a complete cursor traversal form one linear pass,
    // independent of depth, and cards are never duplicated.
    Filter paging; paging.minRating = 8;
    Cursor deep;
    std::set<std::string> completeIds;
    size_t visited = 0;
    for (;;) {
        auto next = old->query(paging, deep, 17, false);
        visited += next.metrics.sortEntries;
        for (const auto& record : next.records) assert(completeIds.insert(record.meta["id"]).second);
        deep = next.cursor;
        if (deep.position == 2504) break;
    }
    assert(completeIds.size() == 2504 && visited == 2504);
    auto finished = old->query(paging, deep, 60, false);
    assert(finished.records.empty() && finished.metrics.sortEntries == 0);
    auto selective = Filter{}; selective.minRating = 9.01; // raw tenths, exact threshold
    assert(old->query(selective, {}, 60, false).total == 1);
    selective.minRating = 9.21;
    assert(old->query(selective, {}, 60, false).total == 0);
    for (auto text : {"alian", "english alias", "TITLE 12", "al", "i", "original 2505"}) {
        auto searchFilter = Filter{}; searchFilter.search = text;
        auto page = old->query(searchFilter, {}, 60, false);
        assert(page.total && page.metrics.sqlFullScanSteps == 0 && page.metrics.sqlSorts == 0);
    }
    // The published disk format has neither the old scan tables nor mutable
    // archive_results. Only integer-keyed visible-record lookups remain.
    sqlite3* inspect = nullptr;
    assert(sqlite3_open_v2(path.c_str(), &inspect, SQLITE_OPEN_READONLY, "gmca-ps4-index") == SQLITE_OK);
    sqlite3_stmt* schema = nullptr;
    assert(sqlite3_prepare_v2(inspect, "SELECT count(*) FROM sqlite_master WHERE name IN ('titles','aliases','archive_results','search_pairs')", -1, &schema, nullptr) == SQLITE_OK);
    assert(sqlite3_step(schema) == SQLITE_ROW && sqlite3_column_int(schema, 0) == 0);
    sqlite3_finalize(schema); sqlite3_close(inspect);

    // A failed replacement must preserve the active index and the reader's generation.
    bool failed = false;
    try { buildImdbIndex(path, cancel, [](const auto&, const auto&, const auto&) { throw std::runtime_error("offline"); }); }
    catch (...) { failed = true; }
    assert(failed && ImdbIndex(path).query({}, {}, 0, false).indexed == 2504);
    assert(buildImdbIndex(path, cancel, fixtures));
    auto freshReader = std::make_shared<ImdbIndex>(path);
    rejectCursor(*freshReader, paging, deep); // cannot carry A cursor into B
    assert(old->query(paging, deep, 60, false).records.empty()); // A survives publish
    assert(testPage(*old, {}, 0, 60, false).total == 2504);
    failed = false;
    try {
        buildImdbIndex(path, cancel, [](const auto&, const auto& output, const auto&) {
            auto gzip = gzopen(output.c_str(), "wb");
            gzputs(gzip, "Not an IMDb dataset\n"); gzclose(gzip);
        });
    } catch (...) { failed = true; }
    assert(failed && !std::filesystem::exists(path + ".building"));
    assert(ImdbIndex(path).query({}, {}, 0, false).indexed == 2504);
    assert(buildImdbIndex(path, cancel, fixtures)); // a clean retry can recover
    // v1 is deliberately incompatible: load rejects it and a clean rebuild
    // replaces it. Published old reader remains alive on its own generation.
    sqlite3* legacy = nullptr;
    assert(sqlite3_open_v2(path.c_str(), &legacy, SQLITE_OPEN_READWRITE, "gmca-ps4-index") == SQLITE_OK);
    assert(sqlite3_exec(legacy, "UPDATE settings SET value=1 WHERE key='version'", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);
    bool incompatible = false;
    try { ImdbIndex rejected(path); } catch (...) { incompatible = true; }
    assert(incompatible);
    assert(buildImdbIndex(path, cancel, fixtures));
    auto reopened = ImdbIndex(path).query({}, {}, 60, false);
    assert(reopened.indexed == 2504 && reopened.records.size() == 60);
    assert(reopened.genres == std::vector<std::string>({"Comedy", "Drama"}));
    // Small adversarial fixture: raw rating vs Bayesian sort, zero years,
    // equal sort keys in both directions and trigrams split across aliases.
    const auto miniPath = (directory / "mini.sqlite").string();
    auto miniData = [](const std::string& dataset, const std::string& output, const IndexCancel&) {
        std::string body;
        if (dataset == "title.ratings.tsv.gz")
            body = "tconst\taverageRating\tnumVotes\n"
                "tt01\t9.5\t100\n" "tt02\t8.0\t1000000\n" "tt03\t5.0\t1000\n";
        else if (dataset == "title.basics.tsv.gz")
            body = "tconst\ttitleType\tprimaryTitle\toriginalTitle\tisAdult\tstartYear\tendYear\truntimeMinutes\tgenres\n"
                "tt01\tmovie\tAlpha\tabc X\t0\t2020\t\\N\t90\tDrama\n"
                "tt02\tmovie\tBeta\tBeta\t0\t2020\t\\N\t90\tDrama\n"
                "tt03\ttvSeries\tGamma\tGamma\t0\t\\N\t\\N\t90\tComedy\n";
        else body = "titleId\tordering\ttitle\tregion\tlanguage\ttypes\tattributes\tisOriginalTitle\n"
                "tt01\t1\tX bcd\tUS\ten\t\\N\t\\N\t0\n"
                "tt02\t1\tABCD percent%_\tIT\tit\t\\N\t\\N\t0\n"
                "tt03\t1\tCaffè\tIT\tit\t\\N\t\\N\t0\n";
        auto gz = gzopen(output.c_str(), "wb"); assert(gz);
        assert(gzwrite(gz, body.data(), body.size()) == int(body.size())); assert(gzclose(gz) == Z_OK);
    };
    assert(buildImdbIndex(miniPath, cancel, miniData));
    ImdbIndex mini(miniPath);
    Filter raw; raw.minRating = 9; raw.sort = Sort::Rating;
    assert(mini.query(raw, {}, 60, false).records[0].meta["id"] == "tt01");
    raw.minRating = 0;
    auto bayes = mini.query(raw, {}, 60, false);
    assert(bayes.records[0].meta["id"] == "tt02" && bayes.records[1].meta["id"] == "tt01");
    raw = {}; raw.search = "abcd";
    auto substring = mini.query(raw, {}, 60, false);
    assert(substring.total == 1 && substring.records[0].meta["id"] == "tt02");
    raw.search = "%_"; assert(mini.query(raw, {}, 60, false).total == 1);
    raw.search = "è"; assert(mini.query(raw, {}, 60, false).total == 1);
    raw.search = "CAFFè"; assert(mini.query(raw, {}, 60, false).total == 1);
    raw = {}; raw.yearTo = 2020; assert(mini.query(raw, {}, 60, false).total == 2);
    raw.yearFrom = 2020;
    for (bool descending : {false, true}) {
        raw.descending = descending;
        auto tied = mini.query(raw, {}, 60, false);
        assert(tied.records[0].meta["id"] == "tt01" && tied.records[1].meta["id"] == "tt02");
    }
    // Cancel during derived-index work and resume without touching published A.
    bool derivedPaused = false;
    int completedStages = 0;
    assert(!buildImdbIndex(miniPath, cancel, miniData, [&](size_t n) {
        if (n == 3 && ++completedStages == 3) { derivedPaused = true; cancel->store(true); }
    }));
    assert(derivedPaused && mini.query({}, {}, 60, false).total == 3);
    cancel->store(false); assert(buildImdbIndex(miniPath, cancel, miniData));

    // Park a real staging build mid-derivation, browse the published generation,
    // then finish the same invocation in background mode (no restart/download).
    PlaybackGate background(std::chrono::milliseconds(0));
    auto backgroundCancel = background.start();
    std::promise<void> parked;
    std::atomic_bool requested{false};
    uint64_t player = 0;
    size_t derivedStages = 0, yields = 0;
    int beforeDownloads = downloads;
    auto building = std::async(std::launch::async, [&] {
        const bool result = buildImdbIndex(path, backgroundCancel, fixtures, [&](size_t count) {
            if (count == 2000 && ++derivedStages == 2) {
                player = background.beginPlayback();
                requested = true;
                parked.set_value();
            }
        }, [&] { ++yields; background.checkpoint(backgroundCancel); });
        background.finish(backgroundCancel);
        return result;
    });
    assert(parked.get_future().wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    background.wait();
    assert(requested && !backgroundCancel->load());
    assert(building.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    assert(ImdbIndex(path).query({}, {}, 60, false).total == 2504);
    background.playbackState(player, true);
    assert(building.get() && yields > 100 && downloads == beforeDownloads + 3);
    assert(ImdbIndex(path).query({}, {}, 60, false).total == 2504);

    // Shutdown while a new build is parked must wake and return false; the old
    // generation is still readable, and the staged datasets need not restart.
    background.playbackState(player, false);
    backgroundCancel = background.start();
    building = std::async(std::launch::async, [&] {
        bool result = buildImdbIndex(path, backgroundCancel, fixtures, {},
            [&] { background.checkpoint(backgroundCancel); });
        background.finish(backgroundCancel);
        return result;
    });
    background.wait();
    assert(building.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    background.stop();
    assert(building.wait_for(std::chrono::seconds(10)) == std::future_status::ready && !building.get());
    assert(ImdbIndex(path).query({}, {}, 60, false).total == 2504);

    old.reset();
    std::filesystem::remove_all(directory);
    std::cout << "IMDb disk index tests passed\n";
}
