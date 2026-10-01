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
    std::puts("playback resume: PASS");
}
