/*
    Pure helpers for Stremio Anime catalog discovery and routed catalog keys.
    Kept independent of the UI and addon transport so the classification and
    route contracts can be checked in a small desktop test.
*/

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace stremio {

enum class AnimeCatalogKind { None, Dedicated, GenreFiltered };

inline bool asciiWordChar(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/// Match `anime` as a case-insensitive whole word in a catalog id or name.
/// Punctuation, spaces, and underscores delimit words; `animation` and
/// `myanimecatalog` do not qualify.
inline bool hasAnimeWord(const std::string& value) {
    constexpr char word[] = {'a', 'n', 'i', 'm', 'e'};
    for (std::size_t i = 0; i + 5 <= value.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < 5; ++j) {
            unsigned char c = static_cast<unsigned char>(value[i + j]);
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != word[j]) {
                match = false;
                break;
            }
        }
        if (match && (i == 0 || !asciiWordChar(static_cast<unsigned char>(value[i - 1]))) &&
            (i + 5 == value.size() || !asciiWordChar(static_cast<unsigned char>(value[i + 5]))))
            return true;
    }
    return false;
}

/// Include native Anime catalogs, explicitly named Anime catalogs, or a
/// generic catalog that advertises and accepts the exact Anime genre filter.
inline AnimeCatalogKind classifyAnimeCatalog(const std::string& catalogType, const std::string& id,
    const std::string& name, const std::vector<std::string>& genres, bool supportsGenreFilter) {
    if (catalogType == "anime" || hasAnimeWord(id) || hasAnimeWord(name)) return AnimeCatalogKind::Dedicated;
    if (supportsGenreFilter) {
        for (const auto& genre : genres)
            if (genre == "Anime") return AnimeCatalogKind::GenreFiltered;
    }
    return AnimeCatalogKind::None;
}

struct CatalogRoute {
    std::string base;
    std::string type;
    std::string id;
    std::string genre;
};

/// Existing routes remain exactly `base\ttype\tcatalogId`. A genre-filtered
/// route adds one optional fourth field, preserving old section and hub keys.
inline std::string makeCatalogRouteKey(const CatalogRoute& route) {
    std::string key = route.base + "\t" + route.type + "\t" + route.id;
    if (!route.genre.empty()) key += "\t" + route.genre;
    return key;
}

inline bool parseCatalogRouteKey(const std::string& key, CatalogRoute& route) {
    const std::size_t first = key.find('\t');
    if (first == std::string::npos) return false;
    const std::size_t second = key.find('\t', first + 1);
    if (second == std::string::npos) return false;
    const std::size_t third = key.find('\t', second + 1);
    route.base = key.substr(0, first);
    route.type = key.substr(first + 1, second - first - 1);
    if (third == std::string::npos) {
        route.id = key.substr(second + 1);
        route.genre.clear();
        return true;
    }
    if (key.find('\t', third + 1) != std::string::npos) return false;
    route.id = key.substr(second + 1, third - second - 1);
    route.genre = key.substr(third + 1);
    return true;
}

}  // namespace stremio
