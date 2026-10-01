#pragma once
#include "api/media/types.hpp"
#include "api/stremio/playback_completion.hpp"
#include <string>
#include <vector>
namespace stremio {
inline bool episodeCompleted(const media::Item& episode) {
    return episode.viewCount > 0 || completedPosition(episode.viewOffset, episode.duration);
}
inline bool episodePartial(const media::Item& episode) {
    return !episodeCompleted(episode) && episode.viewOffset > 0;
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
} // namespace stremio
