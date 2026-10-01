#pragma once
#include <nlohmann/json.hpp>
#include <string>
namespace stremio {
inline std::string subtitleIdentity(const std::string& language, const std::string& id, const std::string& url) {
    return nlohmann::json::array({language, id, url}).dump();
}
} // namespace stremio
