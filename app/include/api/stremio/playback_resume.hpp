#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <algorithm>
#include "api/media/types.hpp"
namespace stremio {
// Identities contain the exact configured provider resource request and a
// release identity. A persisted playback URL alone never selects a provider.
inline std::string savedProviderRequest(const std::string& savedIdentity) {
    try {
        auto identity = nlohmann::json::parse(savedIdentity);
        if (identity.is_array() && identity.size() == 2 && identity[0].is_string() && identity[1].is_string())
            return identity[0].get<std::string>();
    } catch (...) {}
    return {};
}
// A saved release may rotate to the front only for a successful direct resume.
// Before a full picker/fallback, reconstruct the original addon response order.
inline void restoreAddonSourceOrder(std::vector<media::Media>& sources) {
    std::stable_sort(sources.begin(), sources.end(), [](const media::Media& a, const media::Media& b) {
        return a.sourceProviderOrder != b.sourceProviderOrder
            ? a.sourceProviderOrder < b.sourceProviderOrder : a.sourceReleaseOrder < b.sourceReleaseOrder;
    });
}

// A known local zero is authoritative (restart/clear); a remote-only card's
// offset survives metadata refresh, which contains no account movie progress.
inline int64_t resumePosition(int64_t requested, int64_t detail, bool hasLocalRecord) {
    return requested == 0 ? 0 : (hasLocalRecord || detail > 0 ? detail : requested);
}
} // namespace stremio
