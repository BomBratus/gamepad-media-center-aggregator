// Standalone Anime catalog classification, route compatibility, and Stremio
// identity checks. This exercises the parser and key codec used by the backend.

#include <cstdio>
#include <api/stremio/catalog_navigation.hpp>
#include <api/stremio/types.hpp>

static int failures = 0;
#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (!(cond)) {                                                                                \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__);                                    \
            ++failures;                                                                               \
        }                                                                                             \
    } while (0)

int main() {
    using stremio::AnimeCatalogKind;

    CHECK(stremio::classifyAnimeCatalog("movie", "top", "Popular", {}, true) == AnimeCatalogKind::None);
    CHECK(stremio::classifyAnimeCatalog("series", "animation", "Animation", {"Animation"}, true) ==
        AnimeCatalogKind::None);
    CHECK(stremio::classifyAnimeCatalog("movie", "myanimecatalog", "Popular", {}, true) ==
        AnimeCatalogKind::None);
    CHECK(stremio::classifyAnimeCatalog("series", "top_anime", "Popular", {}, false) ==
        AnimeCatalogKind::Dedicated);
    CHECK(stremio::classifyAnimeCatalog("movie", "top", "My Anime Collection", {}, false) ==
        AnimeCatalogKind::Dedicated);
    CHECK(stremio::classifyAnimeCatalog("series", "ANIME", "Popular", {}, false) ==
        AnimeCatalogKind::Dedicated);
    CHECK(stremio::classifyAnimeCatalog("movie", "anime-top", "Popular", {"Anime"}, true) ==
        AnimeCatalogKind::Dedicated);
    CHECK(stremio::classifyAnimeCatalog("movie", "top", "Popular", {"Anime"}, false) ==
        AnimeCatalogKind::None);
    CHECK(stremio::classifyAnimeCatalog("movie", "top", "Popular", {"Anime"}, true) ==
        AnimeCatalogKind::GenreFiltered);

    // A descriptor whose native Stremio type is anime remains eligible even
    // when its catalog id and provider name do not contain the word Anime.
    const auto nativeAnimeManifest = stremio::parseManifest(nlohmann::json::parse(R"({
        "id":"test.addon",
        "resources":["catalog"],
        "catalogs":[{"type":"anime","id":"trending","name":"Kitsu"}]
    })"));
    CHECK(nativeAnimeManifest.catalogs.size() == 1);
    if (!nativeAnimeManifest.catalogs.empty()) {
        const auto& catalog = nativeAnimeManifest.catalogs.front();
        CHECK(catalog.type == "anime");
        CHECK(stremio::classifyAnimeCatalog(catalog.type, catalog.id, catalog.name, catalog.genres,
                  catalog.hasGenre()) == AnimeCatalogKind::Dedicated);
    }

    // Generic catalogs need both the exact Anime option and a genre extra that
    // can accept the filter. Animation by itself does not qualify.
    const auto filterable = stremio::parseManifest(nlohmann::json::parse(R"({
        "id":"test.genre",
        "catalogs":[{"type":"series","id":"top","name":"Popular",
            "extra":[{"name":"genre","options":["Anime","Animation"]}]}]
    })"));
    CHECK(filterable.catalogs.size() == 1);
    if (!filterable.catalogs.empty()) {
        const auto& catalog = filterable.catalogs.front();
        CHECK(catalog.hasGenre());
        CHECK(stremio::classifyAnimeCatalog(catalog.type, catalog.id, catalog.name, catalog.genres,
                  catalog.hasGenre()) == AnimeCatalogKind::GenreFiltered);
    }

    const auto animationOnly = stremio::parseManifest(nlohmann::json::parse(R"({
        "id":"test.animation",
        "catalogs":[{"type":"movie","id":"top","name":"Popular",
            "extra":[{"name":"genre","options":["Animation"]}]}]
    })"));
    CHECK(animationOnly.catalogs.size() == 1);
    if (!animationOnly.catalogs.empty()) {
        const auto& catalog = animationOnly.catalogs.front();
        CHECK(stremio::classifyAnimeCatalog(catalog.type, catalog.id, catalog.name, catalog.genres,
                  catalog.hasGenre()) == AnimeCatalogKind::None);
    }

    // Existing three-field keys serialize and parse unchanged. Anime genre
    // routes add one optional fourth field for pagination and hub navigation.
    const stremio::CatalogRoute legacy{"https://addon.example", "series", "top", ""};
    const std::string legacyKey = stremio::makeCatalogRouteKey(legacy);
    CHECK(legacyKey == "https://addon.example\tseries\ttop");
    stremio::CatalogRoute decoded;
    decoded.genre = "stale";
    CHECK(stremio::parseCatalogRouteKey(legacyKey, decoded));
    CHECK(decoded.base == legacy.base && decoded.type == legacy.type && decoded.id == legacy.id);
    CHECK(decoded.genre.empty());

    const stremio::CatalogRoute anime{"https://addon.example", "movie", "popular", "Anime"};
    const std::string animeKey = stremio::makeCatalogRouteKey(anime);
    CHECK(stremio::parseCatalogRouteKey(animeKey, decoded));
    CHECK(decoded.base == anime.base && decoded.type == anime.type && decoded.id == anime.id);
    CHECK(decoded.genre == "Anime");
    CHECK(!stremio::parseCatalogRouteKey("series\tmissing-second-separator", decoded));
    CHECK(!stremio::parseCatalogRouteKey("base\tseries\ttop\tAnime\textra", decoded));

    // Returned metadata, rather than the catalog descriptor's type, supplies
    // the movie/series identity used by later meta and playback requests.
    const auto movie = stremio::parseMetaPreview(nlohmann::json::parse(R"({
        "type":"movie","id":"kitsu:7442","name":"Movie"
    })"));
    CHECK(movie.type == media::mediaTypeMovie);
    CHECK(movie.ratingKey == "movie:kitsu:7442");
    const auto series = stremio::parseMetaPreview(nlohmann::json::parse(R"({
        "type":"series","id":"kitsu:7442","name":"Series"
    })"));
    CHECK(series.type == media::mediaTypeShow);
    CHECK(series.ratingKey == "series:kitsu:7442");

    // Current dev uses a length-prefixed episode identity for opaque IDs. Keep
    // its parent id and raw video id intact when either includes colons.
    const std::string opaqueEpisode = stremio::episodeId("kitsu:7442", "kitsu:7442:episode:1");
    const auto parsedEpisode = stremio::parseId(opaqueEpisode);
    CHECK(opaqueEpisode.rfind("episode:", 0) == 0);
    CHECK(parsedEpisode.stremioType == "series");
    CHECK(parsedEpisode.baseId == "kitsu:7442");
    CHECK(parsedEpisode.stremioId == "kitsu:7442:episode:1");
    CHECK(parsedEpisode.episode == 0);

    // The historical Cinemeta codec remains recognized for persisted identity.
    const auto legacyEpisode = stremio::parseId("series:tt0903747:2:8");
    CHECK(legacyEpisode.stremioType == "series");
    CHECK(legacyEpisode.stremioId == "tt0903747:2:8");
    CHECK(legacyEpisode.baseId == "tt0903747");
    CHECK(legacyEpisode.season == 2 && legacyEpisode.episode == 8);

    if (failures) {
        std::printf("test_stremio_catalog_navigation: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_stremio_catalog_navigation: OK\n");
    return 0;
}
