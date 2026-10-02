#pragma once
#include "api/media/types.hpp"
#include "api/stremio/playback_completion.hpp"
#include <string>
#include <vector>
namespace stremio {
inline bool episodeCompleted(const media::Item& episode) {
    return episode.viewCount > 0 || completedPosition(episode.viewOffset, episode.duration);
}
// Hydrate either local or account checkpoints with the same completion rule.
inline void applyContinuationCheckpoint(media::Item& item, int64_t position, int64_t duration) {
    if (duration > 0) item.duration = duration;
    item.viewOffset = position;
    item.viewCount = completedPosition(position, item.duration) ? 1 : 0;
}

inline bool episodePartial(const media::Item& episode) {
    return !episodeCompleted(episode) && episode.viewOffset > 0;
}

// A show's newest active checkpoint is sufficient to render its resume card,
// including when the metadata provider is temporarily unavailable.
inline bool resumeCheckpointEpisode(media::Item& show, const std::string& episodeKey) {
    if (episodeKey.empty() || !episodePartial(show)) return false;
    show.key = episodeKey;
    return true;
}

// A missing provider result is not evidence that a series is finished.
// Return false only when a usable episode list has no continuation left.
inline bool applySeriesContinuation(media::Item& show, const media::Item& selected, bool resolvedEpisodes) {
    if (selected.ratingKey.empty()) {
        if (resolvedEpisodes) return false;
        show.key = show.ratingKey;  // open overview until episodes can be resolved
        show.viewOffset = 0;
    } else {
        show.key = selected.ratingKey;
        show.viewOffset = selected.viewOffset;
        show.duration = selected.duration;
    }
    return true;
}

// Episodes are ordered by season/episode by the backend. An explicit current
// key advances strictly forward; entry without a key resumes a partial first.
inline int episodeContinuationIndex(const std::vector<media::Item>& episodes,
    const std::string& currentKey = "") {
    size_t begin = 0;
    if (!currentKey.empty()) {
        bool found = false;
        for (size_t i = 0; i < episodes.size(); ++i)
            if (episodes[i].ratingKey == currentKey) { begin = i + 1; found = true; break; }
        if (!found) return -1;
    }
    for (size_t i = begin; i < episodes.size(); ++i) {
        const auto& e = episodes[i];
        if (episodePartial(e)) return static_cast<int>(i);
    }
    for (size_t i = begin; i < episodes.size(); ++i) {
        const auto& e = episodes[i];
        if (!episodeCompleted(e)) return static_cast<int>(i);
    }
    return -1;
}
// Continue Watching and Next Up follow the latest episode. Older checkpoints
// remain available in the episode list without moving the series backwards.
inline int episodeContinuationFromLatest(const std::vector<media::Item>& episodes,
    const std::string& latestKey) {
    for (size_t i = 0; i < episodes.size(); ++i) {
        if (episodes[i].ratingKey != latestKey) continue;
        if (episodePartial(episodes[i])) return static_cast<int>(i);
        if (episodeCompleted(episodes[i])) return episodeContinuationIndex(episodes, latestKey);
        break;
    }
    return episodeContinuationIndex(episodes);
}
} // namespace stremio
