#include "api/stremio/playback_resume.hpp"
#include "api/stremio/playback_history.hpp"
#include <cassert>
#include <cstdio>
int main() {
    const std::string request = "https://provider.test/config/stream/anime/opaque%3Aepisode.json";
    const std::string identity = nlohmann::json::array({request, "release-hash"}).dump();
    assert(stremio::savedProviderRequest(identity) == request);
    assert(stremio::savedProviderRequest("https://expired.test/video.mp4").empty());
    assert(stremio::savedProviderRequest("[]").empty());
    assert(stremio::savedProviderRequest("[\"provider\",{}]").empty());
    media::Media saved, alternative;
    saved.sourceIdentity = identity;
    media::Part part; part.key = "https://fresh.test/new-signed-url.mp4"; saved.parts.push_back(part);
    alternative.parts.push_back(part); alternative.sourceIdentity = "other-provider-release";
    nlohmann::json record = {{"source", {{"identity", identity}, {"url", "https://expired.test/video.mp4"}}}};
    assert(stremio::PlaybackHistory::chooseSource(record, {alternative, saved}) == 1);
    saved.parts.clear();
    assert(stremio::PlaybackHistory::chooseSource(record, {alternative, saved}) == -1);
    saved.parts.push_back(part);
    part.key = "https://expired.test/video.mp4";
    media::Media ambiguous = saved; ambiguous.parts.front() = part;
    assert(stremio::PlaybackHistory::chooseSource(record, {saved, ambiguous}) == -1);
    ambiguous.sourceIdentity = "wrong-release";
    assert(stremio::PlaybackHistory::chooseSource(record, {ambiguous}) == -1);
    assert(stremio::resumePosition(120000, 0, false) == 120000); // remote-only movie
    assert(stremio::resumePosition(120000, 0, true) == 0); // explicit local clear wins
    assert(stremio::resumePosition(0, 120000, true) == 0); // Restart
    assert(stremio::resumePosition(120000, 150000, false) == 150000);
    media::Media a, b0, b1;
    a.label = "A"; a.sourceProviderOrder = 0;
    b0.label = "B0"; b0.sourceProviderOrder = 1; b0.sourceReleaseOrder = 0;
    b1.label = "B1"; b1.sourceProviderOrder = 1; b1.sourceReleaseOrder = 1;
    std::vector<media::Media> fallback = {b1, b0, a}; // saved B1 first, then lazy A
    stremio::restoreAddonSourceOrder(fallback);
    assert(fallback[0].label == "A" && fallback[1].label == "B0" && fallback[2].label == "B1");
    std::puts("playback resume: PASS");
}
