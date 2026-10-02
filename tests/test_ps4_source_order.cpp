#include "api/stremio/source_safety.hpp"
#include <cassert>
int main() {
    auto source = [](const char* name, const char* codec, const char* resolution, bool playable = true) {
        media::Media m;
        m.label = name; m.videoCodec = codec; m.videoResolution = resolution;
        if (playable) { media::Part p; p.key = name; m.parts.push_back(p); }
        return m;
    };
    std::vector<media::Media> rows = {source("av1", "AV1", "1080p"),
        source("first safe", "H.264", "720p"), source("unknown", "", "1080p"),
        source("second safe", "H.264", "1080p"), source("4k", "H.264", "4K"),
        source("torrent", "H.264", "720p", false), source("hevc", "HEVC", "720p")};
    stremio::orderPs4Sources(rows);
    const char* expected[] = {"first safe", "unknown", "second safe", "av1", "4k", "hevc", "torrent"};
    for (size_t i=0; i<rows.size(); ++i) assert(rows[i].label == expected[i]);
    assert(rows.size() == 7);
    auto hdr = source("hdr", "H.264", "1080p"); hdr.sourceTitle = "Movie HDR10";
    assert(stremio::ps4SafeRank(hdr) < 4);
}
