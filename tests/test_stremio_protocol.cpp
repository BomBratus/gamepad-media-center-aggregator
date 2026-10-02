#include "api/stremio/types.hpp"
#include <cassert>
#include <iostream>

int main() {
    using nlohmann::json;
    auto movie = stremio::parseMeta(json{{"id", "tt123"}, {"type", "movie"},
        {"name", "Movie"}, {"runtime", "120 min"}, {"poster", "https://image.test/poster"}});
    assert(movie.ratingKey == "movie:tt123" && movie.duration == 7200000);
    auto showJson = json{{"id", "tt456"}, {"type", "series"}, {"name", "Series"},
        {"videos", json::array({{{"id", "tt456:1:1"}, {"season", 1}, {"episode", 1}, {"title", "First"}},
                               {{"id", "tt456:1:2"}, {"season", 1}, {"episode", 2}, {"title", "Next"}}})}};
    auto show = stremio::parseMeta(showJson);
    auto episodes = stremio::parseEpisodes(showJson, show);
    assert(episodes.size() == 2 && episodes[1].index == 2);
    assert(episodes[0].grandparentRatingKey == show.ratingKey);
    auto catalog = stremio::parseCatalog(json{{"metas", json::array({{{"id", "tt123"},
        {"type", "movie"}, {"name", "Movie"}}})}});
    assert(catalog.items.size() == 1 && catalog.items[0].ratingKey == movie.ratingKey);
    auto streams = stremio::parseStreams(json{{"streams", json::array({
        {{"name", "HTTP 720p"}, {"url", "https://video.test/movie.mp4"}},
        {{"name", "Torrent"}, {"infoHash", "abc123"}},
        {{"name", "External"}, {"externalUrl", "https://browser.test"}}})}});
    assert(streams.size() == 3);
    auto playable = stremio::streamToMedia(streams[0], "Addon");
    assert(playable.playable() && playable.parts[0].key == streams[0].url);
    assert(!stremio::streamToMedia(streams[1], "Addon").playable());
    assert(!stremio::streamToMedia(streams[2], "Addon").playable());
    auto subs = stremio::parseSubtitles(json{{"subtitles", json::array({
        {{"id", "en"}, {"url", "https://subs.test/en.srt"}, {"lang", "eng"}},
        {{"id", "bad"}, {"lang", "eng"}}})}});
    assert(subs.size() == 1 && subs[0].url == "https://subs.test/en.srt");
    movie.viewOffset = 42000;
    movie.viewCount = 1;
    movie.media = {playable};
    auto restored = json(movie).get<media::Item>();
    assert(restored.viewOffset == 42000 && restored.played());
    assert(restored.media[0].parts[0].key == streams[0].url);
    std::cout << "test_stremio_protocol: OK\n";
}
