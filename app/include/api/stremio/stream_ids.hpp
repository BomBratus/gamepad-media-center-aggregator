#pragma once

#include "api/stremio/types.hpp"

namespace stremio {

// Keep catalog/history identity intact; use explicit movie mappings only for
// source lookup. Series mappings need episode offsets and cannot be substituted.
inline std::vector<std::string> streamLookupIds(
    const std::string& type, const std::string& id, const nlohmann::json& meta) {
    std::vector<std::string> ids{id};
    if (type != "movie" || id.rfind("kitsu:", 0) != 0) return ids;
    const std::string imdb = jstr(meta, "imdb_id", jstr(meta, "imdbId"));
    if (imdb.size() > 2 && imdb.rfind("tt", 0) == 0 &&
        std::all_of(imdb.begin() + 2, imdb.end(), [](char c) { return c >= '0' && c <= '9'; }))
        ids.push_back(imdb);
    return ids;
}

} // namespace stremio
