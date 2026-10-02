#include "api/stremio/catalog_aggregation.hpp"
#include "api/stremio/catalog_navigation.hpp"
#include <cassert>
#include <string>
int main() {
    const std::vector<std::vector<std::string>> catalogs = {
        {"movie1", "movie2", "movie3", "movie4", "movie5"},
        {"series1", "series2"}, {"anime1", "series1"}};
    auto interleaved = stremio::interleaveCatalogs(catalogs, [](const std::string& route) { return route; });
    assert((interleaved == std::vector<std::string>{"movie1", "series1", "anime1", "movie2", "series2", "movie3", "movie4", "movie5"}));
    // The first three requests include all principal categories; duplicate
    // synthetic routes never cause another request. Empty categories are fine.
    auto movieOnly = stremio::interleaveCatalogs(std::vector<std::vector<std::string>>{{"a","b"},{},{}},
        [](const std::string& route) { return route; });
    assert((movieOnly == std::vector<std::string>{"a", "b"}));
    auto genre = stremio::makeCatalogRouteKey({"https://second-provider", "anime", "custom", "Isekai"});
    stremio::CatalogRoute route;
    assert(stremio::parseCatalogRouteKey(genre, route));
    assert(route.base == "https://second-provider" && route.type == "anime" && route.id == "custom" && route.genre == "Isekai");
}
