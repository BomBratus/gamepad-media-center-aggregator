#include "api/stremio/archive_storage.hpp"
#include "api/stremio/archive_catalog.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>

using namespace stremio::archive;

int main() {
    auto required = stremio::parseCatalogDescriptor({{"type", "movie"}, {"id", "year"},
        {"extra", {{{"name", "genre"}, {"isRequired", true}, {"options", {"2026", "2025", "2026"}}}, {{"name", "skip"}}}},
        {"genres", {"2026", "2025"}}});
    assert(catalogVariants(required) == std::vector<std::string>({"2026", "2025"}));
    assert(catalogUrl("https://addon.test/manifest.json", required, "2025", 200) ==
        "https://addon.test/catalog/movie/year/genre=2025&skip=200.json");
    required.genreRequired = false;
    assert(catalogVariants(required) == std::vector<std::string>({"", "2026", "2025"}));
    assert(catalogUrl("https://addon.test/manifest.json", required, "Sci-Fi & Fantasy", 0).find("genre=Sci-Fi%20%26%20Fantasy.json") != std::string::npos);
    auto legacy = stremio::parseCatalogDescriptor({{"extraRequired", {"genre"}}, {"extraSupported", {"genre"}}, {"genres", {"2020"}}});
    assert(catalogVariants(legacy) == std::vector<std::string>({"2020"}));
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
    snapshot.crawlVersion = 2;
    snapshot.crawl = {{"year2025", {{"skip", 200}, {"lastPage", "[tt1,tt2]"}, {"done", false}}},
        {"top", {{"skip", 100}, {"done", true}}}};
    writeSnapshot(path, snapshot);
    auto read = readSnapshot(path);
    assert(read.crawlVersion == 2 && !read.crawlFinished && read.crawl == snapshot.crawl);
    // Old caches retain metadata and are eligible for the expanded crawl.
    auto oldJson = Json{{"version", 1}, {"refreshed", 123}, {"records", Json::array({serialize(records[0])})}};
    auto oldPath = (dir / "old.json").string();
    std::ofstream(oldPath) << oldJson.dump();
    auto old = readSnapshot(oldPath);
    assert(old.records.size() == 1 && old.crawlVersion == 0 && !old.crawlFinished);
    assert(read.refreshed == 123456 && read.partial && read.records.size() == 300);
    assert(select(read.records, threshold) == std::vector<size_t>{148});
    // Both old cutoffs are gone, including on reload after an app restart.
    {
        Snapshot large;
        large.records.reserve(50001);
        for (size_t i = 0; i < 50001; ++i)
            large.records.push_back({{{"id", "tt" + std::to_string(i)}, {"type", "movie"}}, {}, 0, 0});
        auto largePath = (dir / "large.json").string();
        writeSnapshot(largePath, large);
        auto restored = readSnapshot(largePath);
        assert(restored.records.size() == 50001);
        assert(media::jstr(restored.records.back().meta, "id") == "tt50000");
    }
    {
        Snapshot large;
        large.records.push_back(records[0]);
        large.records[0].meta["description"] = std::string(64 * 1024 * 1024 + 1, 'x');
        auto largePath = (dir / "large-bytes.json").string();
        writeSnapshot(largePath, large);
        assert(std::filesystem::file_size(largePath) > 64 * 1024 * 1024);
        auto restored = readSnapshot(largePath);
        assert(restored.records[0].meta["description"] == large.records[0].meta["description"]);
    }
    Snapshot invalid = snapshot;
    // A failure midway through streamed serialization must preserve the old file.
    invalid.records.back().meta["description"] = std::string(1, static_cast<char>(0xff));
    bool threw = false;
    try { writeSnapshot(path, invalid); } catch (...) { threw = true; }
    assert(threw && readSnapshot(path).records.size() == 300);
    assert(!std::filesystem::exists(path + ".tmp"));
    std::ofstream(path) << "broken json";
    threw = false;
    try { readSnapshot(path); } catch (...) { threw = true; }
    assert(threw);
    std::filesystem::remove_all(dir);
    std::cout << "Archive filtering, full-set Random, and cache recovery passed\n";
}
