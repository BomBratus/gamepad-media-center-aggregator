#pragma once
#include "api/stremio/types.hpp"

namespace stremio::archive {
inline std::vector<std::string> catalogVariants(const Catalog& catalog) {
    std::vector<std::string> variants;
    if (!catalog.genreRequired) variants.push_back("");
    if (catalog.hasGenre())
        for (const auto& genre : catalog.genres)
            if (!genre.empty() && std::find(variants.begin(), variants.end(), genre) == variants.end()) variants.push_back(genre);
    return variants;
}
inline std::string catalogUrl(const std::string& transport, const Catalog& catalog, const std::string& genre, size_t skip) {
    std::string url = baseFromTransport(transport) + "/catalog/" + catalog.type + "/" + encodeURIComponent(catalog.id);
    std::string extras;
    if (!genre.empty()) extras = "genre=" + encodeURIComponent(genre);
    if (skip) { if (!extras.empty()) extras += "&"; extras += "skip=" + std::to_string(skip); }
    if (!extras.empty()) url += "/" + extras;
    return url + ".json";
}
} // namespace stremio::archive
