#pragma once
#include <nlohmann/json.hpp>
#include <string>
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
} // namespace stremio
