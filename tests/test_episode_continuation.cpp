#include "api/stremio/episode_continuation.hpp"
#include "api/stremio/types.hpp"
#include <cassert>
#include <cstdio>
int main() {
    auto show = stremio::parseMeta(nlohmann::json::parse(R"({"id":"opaque:show","type":"anime","name":"Anime"})"));
    auto meta = nlohmann::json::parse(R"({"videos":[
        {"id":"special:opaque","season":0,"episode":1},
        {"id":"first:opaque","season":1,"episode":1},
        {"id":"second:opaque","season":1,"episode":2},
        {"id":"next:opaque","season":2,"episode":1}]})");
    auto episodes = stremio::parseEpisodes(meta, show);
    assert(episodes.size() == 4);
    for (const auto& episode : episodes) {
        auto id = stremio::parseId(episode.ratingKey);
        assert(id.stremioType == "anime" && id.baseId == "opaque:show");
        assert(id.stremioId == episode.guid);
    }
    assert(stremio::episodeContinuationIndex(episodes) == 0); // specials preserved
    episodes[2].viewOffset = 1500; episodes[2].duration = 10000;
    assert(stremio::episodeContinuationIndex(episodes) == 2); // partial takes priority
    assert(stremio::episodeContinuationIndex(episodes, episodes[2].ratingKey) == 3); // cross season
    episodes[2].viewCount = 1; episodes[2].viewOffset = 0;
    assert(stremio::episodeContinuationIndex(episodes, episodes[1].ratingKey) == 3); // skip watched
    episodes[3].viewOffset = 9000; episodes[3].duration = 10000;
    assert(stremio::episodeContinuationIndex(episodes, episodes[1].ratingKey) == -1); // remote completion threshold
    episodes[3].viewCount = 1;
    assert(stremio::episodeContinuationIndex(episodes, episodes[1].ratingKey) == -1);
    episodes[0].viewCount = episodes[1].viewCount = 1;
    assert(stremio::episodeContinuationIndex(episodes) == -1); // series finished
    assert(stremio::episodeContinuationIndex(episodes, "missing") == -1);
    auto season = stremio::parseId(stremio::seasonId(show.guid, 0, "anime"));
    assert(season.stremioType == "anime" && season.baseId == show.guid && season.season == 0);
    assert(stremio::stremioType(show) == "anime");
    assert(stremio::playbackResourceKey("series:tt123:1:3") == stremio::playbackResourceKey("series:tt123:1:4"));
    assert(stremio::playbackResourceKey(episodes[0].ratingKey) == stremio::playbackResourceKey(episodes[3].ratingKey));
    assert(stremio::playbackResourceKey("movie:tt123") != stremio::playbackResourceKey("series:tt123:1:3"));
    auto remoteOnly = episodes;
    for (auto& e : remoteOnly) { e.viewCount = 0; e.viewOffset = 0; }
    stremio::applyContinuationCheckpoint(remoteOnly[2], stremio::remotePlaybackOffset(true, true, 10000), 10000);
    assert(stremio::episodeCompleted(remoteOnly[2]));
    assert(stremio::episodeContinuationIndex(remoteOnly, remoteOnly[2].ratingKey) == 3);
    assert(stremio::remotePlaybackOffset(false, true, 9000) == 0);
    // The Wire: manually moving from E4 to E6 must retain E6 at 40 min,
    // without fetching metadata or selecting an older partial episode.
    auto wire = stremio::parseMeta(nlohmann::json::parse(
        R"({"id":"tt0306414","type":"series","name":"The Wire"})"));
    wire.viewOffset = 40 * 60 * 1000;
    wire.duration = 60 * 60 * 1000;
    const auto sixth = stremio::episodeId(wire.guid, "tt0306414:1:6", "series");
    assert(stremio::resumeCheckpointEpisode(wire, sixth));
    assert(wire.key == sixth && wire.viewOffset == 2400000);
    assert(!stremio::resumeCheckpointEpisode(wire, ""));
    wire.viewOffset = 0;
    assert(!stremio::resumeCheckpointEpisode(wire, sixth));
    wire.viewOffset = wire.duration;
    assert(!stremio::resumeCheckpointEpisode(wire, sixth));
    // Addon failure must keep a series visible and route it to its overview;
    // a confirmed finished series should still disappear.
    media::Item unavailable;
    assert(stremio::applySeriesContinuation(wire, unavailable, false));
    assert(wire.key == wire.ratingKey && wire.viewOffset == 0);
    assert(!stremio::applySeriesContinuation(wire, unavailable, true));
    media::Item resolved;
    resolved.ratingKey = sixth;
    resolved.viewOffset = 2400000;
    resolved.duration = 3600000;
    assert(stremio::applySeriesContinuation(wire, resolved, true));
    assert(wire.key == sixth && wire.viewOffset == 2400000 && wire.duration == 3600000);
    std::puts("episode continuation: PASS");
}
