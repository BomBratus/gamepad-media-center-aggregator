// Separate, opt-in realistic Archive workload. No tight wall-clock CI gates.
#include "api/stremio/imdb_index.hpp"
#include <zlib.h>
#include <sqlite3.h>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <cassert>
#include <set>
#include <unistd.h>
using namespace stremio::archive;
using Clock = std::chrono::steady_clock;
#ifdef GMCA_BENCH_LEGACY
static IndexResult first(ImdbIndex& index, const Filter& f, size_t n = 60, bool random = false) {
    return index.query(f, 0, n, random);
}
#else
static IndexResult first(ImdbIndex& index, const Filter& f, size_t n = 60, bool random = false) {
    return index.query(f, {}, n, random);
}
#endif
int main(int argc, char** argv) {
    const int count = argc > 1 ? std::stoi(argv[1]) : 500000;
    const auto dir = std::filesystem::temp_directory_path() / ("gmca-bench-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const auto path = (dir / "index.sqlite").string();
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto fixture = [count](const std::string& dataset, const std::string& output, const IndexCancel&) {
        auto gz = gzopen(output.c_str(), "wb1"); assert(gz);
        const char* header = dataset == "title.ratings.tsv.gz" ? "tconst\taverageRating\tnumVotes\n" :
            dataset == "title.basics.tsv.gz" ? "tconst\ttitleType\tprimaryTitle\toriginalTitle\tisAdult\tstartYear\tendYear\truntimeMinutes\tgenres\n" :
            "titleId\tordering\ttitle\tregion\tlanguage\ttypes\tattributes\tisOriginalTitle\n";
        gzputs(gz, header);
        for (int i = 0; i < count; ++i) {
            char id[32]; std::snprintf(id, sizeof(id), "tt%09d", i);
            std::string row;
            if (dataset == "title.ratings.tsv.gz")
                row = std::string(id) + "\t" + std::to_string((50 + i % 51) / 10.0) + "\t" + std::to_string(100 + (i * int64_t(7919)) % 2000000) + "\n";
            else if (dataset == "title.basics.tsv.gz")
                row = std::string(id) + (i % 4 ? "\tmovie\t" : "\ttvSeries\t") + "Common Film " + std::to_string(i) +
                    "\tOriginal " + std::to_string(i) + "\t0\t" + std::to_string(1950 + i % 77) + "\t\\N\t90\t" +
                    (i % 3 ? "Drama,Comedy" : "Action") + "\n";
            else if (i % 100 == 0)
                row = std::string(id) + "\t1\tItalian Alias " + std::to_string(i) + "\tIT\tit\t\\N\t\\N\t0\n";
            if (!row.empty()) assert(gzwrite(gz, row.data(), row.size()) == int(row.size()));
        }
        assert(gzclose(gz) == Z_OK);
    };
    auto began = Clock::now();
#ifdef GMCA_BENCH_LEGACY
    assert(buildImdbIndex(path, cancel, fixture));
#else
    assert(buildImdbIndex(path, cancel, fixture, {}, {}, [](const std::string& phase, double ms) {
        std::cout << "build-phase name=" << phase << " ms=" << ms << std::endl;
    }));
#endif
    std::cout << "build titles=" << count << " ms=" << std::chrono::duration<double,std::milli>(Clock::now()-began).count()
              << " disk-bytes=" << std::filesystem::file_size(path) << std::endl;
    ImdbIndex index(path);
    auto measure = [&](const char* name, Filter f, bool random = false) {
        auto start = Clock::now(); auto result = first(index, f, 60, random);
        std::cout << name << " ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count()
                  << " matches=" << result.total << " items=" << result.records.size();
#ifndef GMCA_BENCH_LEGACY
        auto& m = result.metrics;
        std::cout << " facet-words=" << m.facetWords << " postings=" << m.searchPostings << " candidates=" << m.searchCandidates
                  << " sort-entries=" << m.sortEntries << " metadata=" << m.metadataRows << " reuse=" << m.reusedMatches;
        assert(m.metadataRows <= (random ? 1 : 60));
        assert(m.sqlFullScanSteps == 0 && m.sqlSorts == 0);
#endif
        std::cout << std::endl; return result;
    };
    Filter f;
    measure("none-cold", f); measure("none-warm", f);
    f.type="movie"; measure("type", f);
    f={}; f.genre="Drama"; measure("genre",f);
    f={}; f.yearFrom=2020; f.yearTo=2026; measure("year",f);
    f={}; f.minRating=8; measure("rating",f);
    f={}; f.minVotes=10000; measure("votes",f);
    f.type="movie"; f.genre="Drama"; f.yearFrom=2020; f.yearTo=2026; f.minRating=8;
    auto combination = measure("combined",f);
    f.sort=Sort::Name; auto sorted=measure("sort-change",f);
#ifndef GMCA_BENCH_LEGACY
    assert(sorted.metrics.reusedMatches && sorted.metrics.facetWords==0);
#endif
    f={}; f.search="Common"; measure("search-common",f);
    f.search="absolutely-absent"; measure("search-none",f);
    f.search="1"; measure("search-short",f);
    f={}; measure("random-cold", f,true); measure("random-warm",f,true);
    auto page1=measure("page-1",f);
    auto start=Clock::now();
#ifdef GMCA_BENCH_LEGACY
    auto deep=index.query(f, 60000, 60, false);
#else
    Cursor cursor=page1.cursor;
    std::set<std::string> ids;
    size_t positions=page1.metrics.sortEntries;
    for (int i=0; i<999; ++i) {
        auto next=index.query(f,cursor,60,false);
        assert(next.metrics.reusedMatches && next.metrics.facetWords==0);
        positions+=next.metrics.sortEntries; cursor=next.cursor;
    }
    start=Clock::now();
    auto deep=index.query(f,cursor,60,false);
    assert(deep.metrics.reusedMatches && deep.metrics.sortEntries==60);
    assert(positions==60000 && deep.cursor.position==60060);
#endif
    std::cout << "deep-page ms=" << std::chrono::duration<double,std::milli>(Clock::now()-start).count() << " items=" << deep.records.size() << std::endl;
    std::filesystem::remove_all(dir);
}
