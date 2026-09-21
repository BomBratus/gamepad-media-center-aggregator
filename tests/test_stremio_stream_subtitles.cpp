// Standalone logic test — Stremio stream-level subtitle parsing.
//
// Verifies that `stream.subtitles[]` and subtitle-matching behaviorHints
// survive parseStreams()/streamToMedia(), so the backend can query subtitle
// providers for the exact selected release without hashing the remote file.
//
//   c++ -std=gnu++17 -Iapp/include -Ilibrary/borealis/library/include/borealis/extern \
//       tests/test_stremio_stream_subtitles.cpp -o /tmp/t && /tmp/t

#include <cstdio>
#include <api/stremio/types.hpp>

using nlohmann::json;

static int failures = 0;
#define CHECK(cond)                                           \
    do {                                                      \
        if (!(cond)) {                                        \
            std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                       \
        }                                                     \
    } while (0)

int main() {
    json payload = {
        {"streams",
            json::array({
                {
                    {"name", "Example"},
                    {"url", "https://video.example/episode.mkv"},
                    {"behaviorHints",
                        {
                            {"videoHash", "0123456789abcdef"},
                            {"videoSize", 987654321},
                            {"filename", "Example.S01E01.1080p.mkv"},
                        }},
                    {"subtitles",
                        json::array({
                            {{"id", "it-1"}, {"url", "https://subs.example/it.srt"}, {"lang", "ita"}},
                            {{"id", "en-1"}, {"url", "https://subs.example/en.vtt"}, {"lang", "eng"}},
                            {{"id", "broken"}, {"lang", "fra"}},
                        })},
                },
            })},
    };

    auto streams = stremio::parseStreams(payload);
    CHECK(streams.size() == 1);
    CHECK(streams.size() == 1 && streams[0].url == "https://video.example/episode.mkv");
    CHECK(streams.size() == 1 && streams[0].videoHash == "0123456789abcdef");
    CHECK(streams.size() == 1 && streams[0].videoSize == 987654321);
    CHECK(streams.size() == 1 && streams[0].filename == "Example.S01E01.1080p.mkv");
    if (streams.size() == 1) {
        auto media = stremio::streamToMedia(streams[0], "Example Addon");
        CHECK(media.parts.size() == 1);
        CHECK(media.parts.size() == 1 && media.parts[0].videoHash == "0123456789abcdef");
        CHECK(media.parts.size() == 1 && media.parts[0].size == 987654321);
        CHECK(media.parts.size() == 1 && media.parts[0].filename == "Example.S01E01.1080p.mkv");
    }
    CHECK(streams.size() == 1 && streams[0].subtitles.size() == 2);
    CHECK(streams.size() == 1 && streams[0].subtitles.size() == 2 &&
          streams[0].subtitles[0].id == "it-1");
    CHECK(streams.size() == 1 && streams[0].subtitles.size() == 2 &&
          streams[0].subtitles[0].url == "https://subs.example/it.srt");
    CHECK(streams.size() == 1 && streams[0].subtitles.size() == 2 &&
          streams[0].subtitles[0].lang == "ita");
    CHECK(streams.size() == 1 && streams[0].subtitles.size() == 2 &&
          streams[0].subtitles[1].lang == "eng");

    if (failures == 0) {
        std::printf("test_stremio_stream_subtitles: OK\n");
        return 0;
    }
    std::printf("test_stremio_stream_subtitles: %d FAILURE(S)\n", failures);
    return 1;
}
