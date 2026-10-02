#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// Keep the installed configuration schema and unrecognized legacy fields.
struct AppUser {
    std::string id, name, access_token, server_id, thumb;
    nlohmann::json stored = nlohmann::json::object();
};
inline void from_json(const nlohmann::json& j, AppUser& u) {
    u.stored = j;
    u.id = j.value("id", "");
    u.name = j.value("name", "");
    u.access_token = j.value("access_token", "");
    u.server_id = j.value("server_id", "");
    u.thumb = j.value("thumb", "");
}
inline void to_json(nlohmann::json& j, const AppUser& u) {
    j = u.stored;
    for (const auto& field : std::vector<std::pair<const char*, std::string>>{
             {"id", u.id}, {"name", u.name}, {"access_token", u.access_token},
             {"server_id", u.server_id}, {"thumb", u.thumb}}) {
        if (j.contains(field.first) || !field.second.empty()) j[field.first] = field.second;
    }
}

struct AppServer {
    std::string name, id, access_token;
    std::vector<std::string> urls;
    // Missing discriminants belong to legacy records, never to Stremio.
    std::string type;
    std::vector<std::string> addons;
    nlohmann::json stored = nlohmann::json::object();
};
inline void from_json(const nlohmann::json& j, AppServer& s) {
    s.stored = j;
    s.id = j.value("id", "");
    s.name = j.value("name", "");
    s.access_token = j.value("access_token", "");
    s.urls = j.value("urls", std::vector<std::string>{});
    s.type = j.value("type", "");
    s.addons = j.value("addons", std::vector<std::string>{});
}
inline void to_json(nlohmann::json& j, const AppServer& s) {
    if (s.type != "stremio" && !s.stored.empty()) {
        j = s.stored;
        return;
    }
    j = s.stored;
    j["id"] = s.id;
    j["name"] = s.name;
    j["access_token"] = s.access_token;
    j["urls"] = s.urls;
    j["type"] = s.type;
    j["addons"] = s.addons;
}

inline bool supportedStremioAccount(const AppServer& s) {
    return s.type == "stremio" && !s.id.empty() && !s.access_token.empty() &&
           !s.urls.empty() && !s.urls.front().empty();
}

// Ambiguous legacy identifiers must never convert an unsupported account.
inline const AppServer* selectedStremioServer(const std::vector<AppServer>& servers, const std::string& id) {
    const AppServer* selected = nullptr;
    for (const auto& server : servers) {
        if (server.id != id) continue;
        if (!supportedStremioAccount(server)) return nullptr;
        if (!selected) selected = &server;
    }
    return selected;
}
