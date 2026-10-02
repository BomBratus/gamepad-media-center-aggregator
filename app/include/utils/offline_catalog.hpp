#pragma once

/*
    Pure catalog logic for offline browsing (SPEC §4.1).

    Turns a flat set of media::Item snapshots (persisted under downloads/meta/)
    into a navigable libraries -> movies/shows -> seasons -> episodes tree,
    mirroring the online model. Header-only and dependency-free (neutral media types +
    STL) so it can be unit-tested without Borealis/config — the OfflineLibrary
    singleton (I/O, singleton state) builds on top of it.
*/

#include <api/media/types.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace offline {

/// Deterministic, filesystem-safe slug from a title (synthetic keys only).
inline std::string slug(const std::string& s) {
    std::string out;
    for (char c : s) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u))
            out += (char)std::tolower(u);
        else if (c == ' ' || c == '-' || c == '_')
            out += '-';
    }
    return out.empty() ? "x" : out;
}

/// Synthetic ancestor keys for legacy downloads that lack real ratingKeys.
inline std::string syntheticShowKey(const std::string& showTitle) { return "local:show:" + slug(showTitle); }
inline std::string syntheticSeasonKey(const std::string& showKey, int64_t seasonIndex) {
    return showKey + ":s" + std::to_string(seasonIndex);
}

/// A top-level (grid) node is a movie or a show.
inline bool isTopLevel(const media::Item& it) {
    return it.type == media::mediaTypeMovie || it.type == media::mediaTypeShow;
}

/// Owning library key: the real librarySectionID, or a synthetic bucket by
/// media type so navigation still works when the field was not captured.
inline std::string sectionKeyOf(const media::Item& it) {
    if (!it.librarySectionID.empty()) return it.librarySectionID;
    return it.type == media::mediaTypeMovie ? "local:lib:movie" : "local:lib:show";
}

/// Ensure every episode has a show + season node in the set. Missing ancestors
/// are synthesized from the episode's grandparent*/parent* fields (and the
/// episode is re-linked to them) so a legacy download captured before rich
/// metadata is still browsable. Idempotent: real ancestors already present are
/// left untouched.
inline std::vector<media::Item> synthesizeAncestors(std::vector<media::Item> nodes) {
    std::unordered_set<std::string> have;
    for (auto& it : nodes) have.insert(it.ratingKey);

    std::vector<media::Item> added;
    for (auto& it : nodes) {
        if (it.type != media::mediaTypeEpisode) continue;

        std::string showKey = it.grandparentRatingKey;
        if (showKey.empty()) {
            showKey = syntheticShowKey(it.grandparentTitle);
            it.grandparentRatingKey = showKey;
        }
        std::string seasonKey = it.parentRatingKey;
        if (seasonKey.empty()) {
            seasonKey = syntheticSeasonKey(showKey, it.parentIndex);
            it.parentRatingKey = seasonKey;
        }

        if (!have.count(showKey)) {
            media::Item show;
            show.ratingKey = showKey;
            show.type = media::mediaTypeShow;
            show.title = it.grandparentTitle.empty() ? "Unknown" : it.grandparentTitle;
            show.thumb = it.grandparentThumb;
            show.art = it.grandparentArt;
            show.librarySectionID = it.librarySectionID;
            show.librarySectionTitle = it.librarySectionTitle;
            have.insert(showKey);
            added.push_back(std::move(show));
        }
        if (!have.count(seasonKey)) {
            media::Item season;
            season.ratingKey = seasonKey;
            season.type = media::mediaTypeSeason;
            season.parentRatingKey = showKey;
            season.index = it.parentIndex;
            season.title = it.parentTitle.empty() ? ("Season " + std::to_string(it.parentIndex)) : it.parentTitle;
            season.thumb = it.parentThumb.empty() ? it.grandparentThumb : it.parentThumb;
            season.librarySectionID = it.librarySectionID;
            season.librarySectionTitle = it.librarySectionTitle;
            have.insert(seasonKey);
            added.push_back(std::move(season));
        }
    }
    nodes.insert(nodes.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    return nodes;
}

/// Distinct libraries (movies + shows) present, as media::Section, sorted by title.
inline std::vector<media::Section> buildSections(const std::vector<media::Item>& nodes) {
    std::vector<media::Section> out;
    std::unordered_set<std::string> seen;
    for (const auto& it : nodes) {
        if (!isTopLevel(it)) continue;
        std::string key = sectionKeyOf(it);
        if (!seen.insert(key).second) continue;
        media::Section s;
        s.key = key;
        s.type = it.type == media::mediaTypeMovie ? media::mediaTypeMovie : media::mediaTypeShow;
        s.title = !it.librarySectionTitle.empty() ? it.librarySectionTitle
                                                   : (it.type == media::mediaTypeMovie ? "Movies" : "TV Shows");
        out.push_back(std::move(s));
    }
    std::sort(out.begin(), out.end(), [](const media::Section& a, const media::Section& b) { return a.title < b.title; });
    return out;
}

/// Top-level items (movies + shows) of a section, sorted by title.
inline std::vector<media::Item> sectionItems(const std::vector<media::Item>& nodes, const std::string& sectionKey) {
    std::vector<media::Item> out;
    for (const auto& it : nodes)
        if (isTopLevel(it) && sectionKeyOf(it) == sectionKey) out.push_back(it);
    std::sort(out.begin(), out.end(), [](const media::Item& a, const media::Item& b) { return a.title < b.title; });
    return out;
}

/// Offline search over the local catalog: top-level items (movies + shows)
/// whose title contains `term` (case-insensitive), sorted by title. An empty
/// term returns every top-level item (the offline "suggestions" grid). Scope
/// mirrors the online movies,tv search — episodes surface by drilling into a
/// show, never as standalone poster cards (SPEC §4.4).
inline std::vector<media::Item> search(const std::vector<media::Item>& nodes, const std::string& term) {
    auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string needle = lower(term);
    std::vector<media::Item> out;
    for (const auto& it : nodes) {
        if (!isTopLevel(it)) continue;
        if (!needle.empty() && lower(it.title).find(needle) == std::string::npos) continue;
        out.push_back(it);
    }
    std::sort(out.begin(), out.end(), [](const media::Item& a, const media::Item& b) { return a.title < b.title; });
    return out;
}

/// Direct children (seasons of a show, or episodes of a season), sorted by index.
inline std::vector<media::Item> childrenOf(const std::vector<media::Item>& nodes, const std::string& parentRatingKey) {
    std::vector<media::Item> out;
    if (parentRatingKey.empty()) return out;
    for (const auto& it : nodes)
        if (it.parentRatingKey == parentRatingKey) out.push_back(it);
    std::sort(out.begin(), out.end(), [](const media::Item& a, const media::Item& b) { return a.index < b.index; });
    return out;
}

/// All episodes under a show, across seasons (mirrors /allLeaves), sorted by
/// (season index, episode index).
inline std::vector<media::Item> leavesOf(const std::vector<media::Item>& nodes, const std::string& showRatingKey) {
    std::vector<media::Item> out;
    for (const auto& it : nodes)
        if (it.type == media::mediaTypeEpisode && it.grandparentRatingKey == showRatingKey) out.push_back(it);
    std::sort(out.begin(), out.end(), [](const media::Item& a, const media::Item& b) {
        return a.parentIndex != b.parentIndex ? a.parentIndex < b.parentIndex : a.index < b.index;
    });
    return out;
}

/// ratingKeys to KEEP given which leaves are actually downloaded (predicate).
/// Prunes movies/clips not downloaded, shows and seasons with no downloaded
/// episode, and their now-orphaned children; keeps a non-downloaded episode
/// whose season still has a download (greyed sibling). (SPEC AC18)
inline std::unordered_set<std::string> survivors(
    const std::vector<media::Item>& nodes, const std::function<bool(const std::string&)>& isDownloaded) {
    std::unordered_set<std::string> seasonsWithDl, showsWithDl;
    for (const auto& it : nodes) {
        if (it.type == media::mediaTypeEpisode && isDownloaded(it.ratingKey)) {
            if (!it.parentRatingKey.empty()) seasonsWithDl.insert(it.parentRatingKey);
            if (!it.grandparentRatingKey.empty()) showsWithDl.insert(it.grandparentRatingKey);
        }
    }
    std::unordered_set<std::string> keep;
    for (const auto& it : nodes) {
        bool k;
        if (it.type == media::mediaTypeMovie || it.type == media::mediaTypeClip)
            k = isDownloaded(it.ratingKey);
        else if (it.type == media::mediaTypeShow)
            k = showsWithDl.count(it.ratingKey) > 0;
        else if (it.type == media::mediaTypeSeason)
            k = showsWithDl.count(it.parentRatingKey) > 0 && seasonsWithDl.count(it.ratingKey) > 0;
        else if (it.type == media::mediaTypeEpisode)
            k = seasonsWithDl.count(it.parentRatingKey) > 0;
        else
            k = true;  // unknown type: keep defensively
        if (k) keep.insert(it.ratingKey);
    }
    return keep;
}

/// Image paths worth caching for a fiche (non-empty only): poster, backdrop,
/// cut-out logo, parent/grandparent art and every cast face. Fed to
/// ImageCache::store at download time (SPEC §4.2).
inline std::vector<std::string> assetPaths(const media::Item& it) {
    std::vector<std::string> out;
    auto add = [&](const std::string& p) {
        if (!p.empty()) out.push_back(p);
    };
    add(it.thumb);
    add(it.art);
    add(it.clearLogo);
    add(it.parentThumb);
    add(it.grandparentThumb);
    add(it.grandparentArt);
    for (const auto& r : it.roles) add(r.thumb);
    return out;
}

}  // namespace offline
