// Standalone persistence and source matching tests for Stremio playback history.

#include <api/stremio/playback_history.hpp>
#include <api/stremio/types.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                            \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
            ++failures;                                        \
        }                                                      \
    } while (0)

static media::Item makeItem(const std::string& id, const std::string& title) {
    media::Item item;
    item.ratingKey = id;
    item.type = media::mediaTypeEpisode;
    item.title = title;
    item.thumb = "poster.jpg";
    item.guid = "imdb://" + id;
    item.grandparentRatingKey = "series:show-1";
    item.grandparentTitle = "Example Show";
    item.grandparentThumb = "show.jpg";
    item.grandparentArt = "show-art.jpg";
    return item;
}

static media::Media makeSource(const std::string& url, const std::string& identity) {
    media::Media source;
    source.sourceIdentity = identity;
    media::Part part;
    part.key = url;
    source.parts.push_back(part);
    return source;
}

int main() {
    // Exercise the production stream mapper: signed links may rotate, but a
    // different release/addon must not inherit the selected source identity.
    stremio::StreamOption option;
    option.name = "Debrid 1080p";
    option.title = "Example.Show.S01E01.Release";
    option.url = "https://cdn.example/video?token=old";
    auto mapped = stremio::streamToMedia(option, "Addon A");
    option.url = "https://cdn.example/video?token=new";
    CHECK(stremio::streamToMedia(option, "Addon A").sourceIdentity == mapped.sourceIdentity);
    CHECK(stremio::streamToMedia(option, "Addon B").sourceIdentity != mapped.sourceIdentity);
    option.title = "Different.Release";
    CHECK(stremio::streamToMedia(option, "Addon A").sourceIdentity != mapped.sourceIdentity);
    option.title.clear();
    CHECK(stremio::streamToMedia(option, "Addon A").sourceIdentity.empty());

    char directoryTemplate[] = "/tmp/gmca-playback-history-XXXXXX";
    char* directoryName = ::mkdtemp(directoryTemplate);
    CHECK(directoryName != nullptr);
    if (!directoryName) return 1;
    const std::filesystem::path directory(directoryName);
    const std::string path = (directory / "history.json").string();

    const media::Media savedSource = makeSource("https://cdn.example/video?token=old", "addon/release/file");
    {
        stremio::PlaybackHistory history(path);
        CHECK(history.save("account-a", makeItem("series:show-1:1:1", "Episode 1"), savedSource, 90000, 180000));
        // Position snapshots may move backwards, and zero is a valid saved value.
        CHECK(history.save("account-a", makeItem("series:show-1:1:1", "Episode 1"), savedSource, 25000, 180000));
        CHECK(history.save("account-a", makeItem("series:show-1:1:2", "Episode 2"), savedSource, 0, 190000));
        CHECK(history.save("account-b", makeItem("series:show-1:1:1", "Episode 1"), savedSource, 7000, 180000));
    }

    // A fresh instance sees durable data. Account and episode keys do not bleed
    // into one another, and the latest (backward-moving) position wins.
    stremio::PlaybackHistory restarted(path);
    const auto first = restarted.load("account-a", "series:show-1:1:1");
    CHECK(first.is_object());
    CHECK(first.value("position", int64_t{-1}) == 25000);
    CHECK(first.value("duration", int64_t{-1}) == 180000);
    CHECK(restarted.load("account-a", "series:show-1:1:2").value("position", int64_t{-1}) == 0);
    CHECK(restarted.load("account-b", "series:show-1:1:1").value("position", int64_t{-1}) == 7000);
    CHECK(restarted.load("account-a", "missing").empty());
    CHECK(restarted.records("account-a").size() == 2);

    // The saved record includes only the item metadata needed to restore a
    // Continue Watching entry, retaining optional grandparent presentation.
    const auto item = first.value("item", nlohmann::json::object());
    CHECK(item.value("ratingKey", std::string()) == "series:show-1:1:1");
    CHECK(item.value("type", std::string()) == media::mediaTypeEpisode);
    CHECK(item.value("title", std::string()) == "Episode 1");
    CHECK(item.value("thumb", std::string()) == "poster.jpg");
    CHECK(item.value("guid", std::string()) == "imdb://series:show-1:1:1");
    CHECK(item.value("grandparentTitle", std::string()) == "Example Show");
    CHECK(item.value("grandparentArt", std::string()) == "show-art.jpg");
    CHECK(first.value("updated", int64_t{0}) > 0);

    // Local millisecond timestamps compare chronologically with either
    // second-precision or millisecond-precision UTC timestamps from Stremio.
    const nlohmann::json recentRecord = {{"updated", int64_t{1700000000123}}};
    CHECK(stremio::PlaybackHistory::timestampIso(1700000000123) == "2023-11-14T22:13:20.123Z");
    CHECK(stremio::PlaybackHistory::updatedIso(recentRecord) == "2023-11-14T22:13:20.123Z");
    CHECK(stremio::PlaybackHistory::localIsNewer(recentRecord, "2023-11-14T22:13:20.122Z"));
    CHECK(!stremio::PlaybackHistory::localIsNewer(recentRecord, "2023-11-14T22:13:20.124Z"));
    CHECK(stremio::PlaybackHistory::localIsNewer(recentRecord, "2023-11-14T22:13:20Z"));
    const nlohmann::json secondRecord = {{"updated", int64_t{1700000000000}}};
    CHECK(!stremio::PlaybackHistory::localIsNewer(secondRecord, "2023-11-14T22:13:20Z"));
    CHECK(stremio::PlaybackHistory::localIsNewer(recentRecord, ""));
    CHECK(stremio::PlaybackHistory::localIsNewer(recentRecord, "not-a-timestamp"));
    CHECK(stremio::PlaybackHistory::timestampIso(-1) == "1969-12-31T23:59:59.999Z");

    // A refreshed signed URL can still identify the unique matching source;
    // when an exact URL is available, it wins even if another identity matches.
    std::vector<media::Media> refreshed = {
        makeSource("https://cdn.example/other", "another/release/file"),
        makeSource("https://cdn.example/video?token=new", "addon/release/file"),
    };
    CHECK(stremio::PlaybackHistory::chooseSource(first, refreshed) == 1);
    refreshed.insert(refreshed.begin(), makeSource("https://cdn.example/video?token=old", "different/identity"));
    CHECK(stremio::PlaybackHistory::chooseSource(first, refreshed) == 2);

    media::Media placeholder;
    placeholder.sourceIdentity = "addon/release/file";
    std::vector<media::Media> withPlaceholder = {
        placeholder,
        makeSource("https://cdn.example/video?token=renewed", "addon/release/file"),
    };
    CHECK(stremio::PlaybackHistory::chooseSource(first, withPlaceholder) == 1);
    CHECK(stremio::PlaybackHistory::chooseSource(first, {placeholder}) == -1);

    // Repeated stable identities are ambiguous and must not silently select a
    // potentially unrelated source after signed URLs change.
    std::vector<media::Media> duplicateIdentities = {
        makeSource("https://cdn.example/new-1", "addon/release/file"),
        makeSource("https://cdn.example/new-2", "addon/release/file"),
    };
    CHECK(stremio::PlaybackHistory::chooseSource(first, duplicateIdentities) == -1);
    CHECK(stremio::PlaybackHistory::chooseSource(nlohmann::json::object(), refreshed) == -1);

    // Clearing marks playback complete while preserving source information.
    const auto sourceBeforeClear = first.value("source", nlohmann::json::object());
    CHECK(restarted.clear("account-a", "series:show-1:1:1"));
    const auto cleared = restarted.load("account-a", "series:show-1:1:1");
    CHECK(cleared.value("position", int64_t{-1}) == 0);
    CHECK(cleared.value("source", nlohmann::json::object()) == sourceBeforeClear);
    CHECK(cleared.value("duration", int64_t{-1}) == 180000);
    CHECK(!restarted.clear("account-a", "missing"));

    // Corrupt on-disk JSON is treated as an empty history rather than throwing.
    const std::string malformedPath = (directory / "malformed.json").string();
    {
        std::ofstream malformed(malformedPath);
        malformed << "{broken json";
    }
    stremio::PlaybackHistory malformed(malformedPath);
    CHECK(malformed.load("account-a", "id").empty());
    CHECK(malformed.records("account-a").empty());
    CHECK(malformed.save("account-a", makeItem("recovered", "Recovered"), savedSource, 1, 2));
    CHECK(malformed.load("account-a", "recovered").value("position", int64_t{-1}) == 1);

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures == 0) {
        printf("test_stremio_playback_history: OK\n");
        return 0;
    }
    printf("test_stremio_playback_history: %d FAILURE(S)\n", failures);
    return 1;
}
