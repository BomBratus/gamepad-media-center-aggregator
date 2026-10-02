#include "api/stremio/archive_storage.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>

using namespace stremio::archive;

int main() {
    std::vector<Record> records;
    for (int i = 0; i < 300; ++i) {
        records.push_back({{{"id", "tt" + std::to_string(i)}, {"type", i % 2 ? "series" : "movie"},
            {"name", "Title " + std::to_string(i)}, {"releaseInfo", "2020-2024"}, {"imdbRating", "8.0"},
            {"genres", {i % 3 ? "Drama" : "Comedy"}}, {"country", "Italy, France"}, {"views", i * 100},
            {"services", {"Fixture service"}}, {"poster", "https://example.test/poster"}}, {"Test addon"}, i, i + 1});
    }
    Filter filter;
    filter.type = "movie"; filter.genre = "Drama"; filter.country = "Italy"; filter.yearFrom = 2020;
    filter.yearTo = 2020; filter.minRating = 8; filter.minViews = 1000; filter.addon = "Test addon";
    filter.service = "Fixture service";
    auto matching = select(records, filter);
    assert(matching.size() > 60);
    for (auto i : matching) assert(i >= 10 && i % 2 == 0 && i % 3 != 0);
    assert(std::find(matching.begin(), matching.end(), 148) != matching.end());
    std::mt19937 engine(42);
    bool beyondFirstPage = false;
    std::set<size_t> picked;
    for (int i = 0; i < 10000; ++i) {
        auto index = randomMatch(matching, engine);
        assert(matches(records[index], filter));
        picked.insert(index);
        if (std::find(matching.begin(), matching.end(), index) - matching.begin() >= 60) beyondFirstPage = true;
    }
    assert(beyondFirstPage && picked.size() == matching.size());
    assert(randomMatch({}, engine) == size_t(-1));
    Filter missing = filter; missing.country = "Japan";
    assert(select(records, missing).empty());
    missing = filter; missing.service = "Missing service";
    assert(select(records, missing).empty());
    Record absent = {{{"id", "unknown"}, {"type", "movie"}, {"name", "Unknown"}, {"viewCount", 999999}}, {}, 0, 0};
    Filter threshold; threshold.minViews = 1;
    assert(!matches(absent, threshold)); // watched counts are not public views
    threshold = {}; threshold.yearTo = 2025;
    assert(!matches(absent, threshold));
    threshold = {}; threshold.minRating = 7;
    assert(!matches(absent, threshold));
    threshold = {}; threshold.search = "TITLE 148";
    assert(select(records, threshold) == std::vector<size_t>{148});
    filter = {}; filter.sort = Sort::Added; filter.descending = false;
    auto sorted = select(records, filter);
    assert(sorted.front() == 0 && sorted.back() == 299);
    filter.descending = true; sorted = select(records, filter);
    assert(sorted.front() == 299 && sorted.back() == 0);
    filter.sort = Sort::Name; filter.descending = false;
    sorted = select(records, filter);
    assert(sorted.front() == 0 && sorted.back() == 99);
    filter.sort = Sort::Views; filter.descending = true;
    sorted = select(records, filter);
    assert(sorted.front() == 299 && sorted.back() == 0);
    auto dates = records;
    dates[0].meta["released"] = "2020-12-31T00:00:00Z";
    dates[1].meta["released"] = "2020-01-01T00:00:00Z";
    filter.sort = Sort::Release;
    sorted = select(dates, filter);
    assert(sorted.front() == 0 && sorted[1] == 1);

    auto dir = std::filesystem::temp_directory_path() / ("gmca-archive-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    auto path = (dir / "archive.json").string();
    Snapshot snapshot{records, 123456, true};
    writeSnapshot(path, snapshot);
    auto read = readSnapshot(path);
    assert(read.refreshed == 123456 && read.partial && read.records.size() == 300);
    assert(select(read.records, threshold) == std::vector<size_t>{148});
    Snapshot oversized = snapshot;
    oversized.records.front().meta["description"] = std::string(MAX_CACHE_BYTES + 1, 'x');
    bool threw = false;
    try { writeSnapshot(path, oversized); } catch (...) { threw = true; }
    assert(threw && readSnapshot(path).records.size() == 300);
    assert(!std::filesystem::exists(path + ".tmp"));
    std::ofstream(path) << "broken json";
    threw = false;
    try { readSnapshot(path); } catch (...) { threw = true; }
    assert(threw);
    std::filesystem::remove_all(dir);
    std::cout << "Archive filtering, full-set Random, and cache recovery passed\n";
}
