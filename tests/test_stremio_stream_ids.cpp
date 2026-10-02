#include <api/stremio/stream_ids.hpp>
#include <cassert>
#include <cstdio>

int main() {
    using stremio::streamLookupIds;
    const auto meta = nlohmann::json::parse(R"({"id":"kitsu:11614","type":"movie","imdb_id":"tt5311514"})");
    const auto ids = streamLookupIds("movie", "kitsu:11614", meta);
    assert((ids == std::vector<std::string>{"kitsu:11614", "tt5311514"}));
    // An IMDb-only provider becomes eligible for the fallback request.
    stremio::Addon addon;
    addon.manifest = stremio::parseManifest(nlohmann::json::parse(
        R"({"resources":["stream"],"types":["movie"],"idPrefixes":["tt"]})"));
    assert(!addon.supports("stream", "movie", ids[0]));
    assert(addon.supports("stream", "movie", ids[1]));
    assert(streamLookupIds("series", "kitsu:11614:1", meta).size() == 1);
    assert(streamLookupIds("movie", "tt5311514", meta).size() == 1);
    assert(streamLookupIds("movie", "kitsu:11614", nlohmann::json::object()).size() == 1);
    for (const auto& invalid : {"tt", "5311514", "tt5311514:1:1", "tt5311514tail"}) {
        assert(streamLookupIds("movie", "kitsu:11614", {{"imdb_id", invalid}}).size() == 1);
    }
    assert(streamLookupIds("movie", "kitsu:11614", {{"imdb_id", 5311514}}).size() == 1);
    assert(streamLookupIds("movie", "kitsu:11614", {{"imdbId", "tt5311514"}}).size() == 2);
    std::puts("Stremio movie stream ID checks passed");
}
