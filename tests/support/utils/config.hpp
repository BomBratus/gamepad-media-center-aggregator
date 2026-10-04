#pragma once
#include <string>
#include <vector>
#include <mutex>
class AppConfig {
public:
    struct StremioAccount { std::string token; std::vector<std::string> addons; };
    static AppConfig& instance() { static AppConfig config; return config; }
    StremioAccount getStremioAccount() const { return {"test-token", {"https://persisted/manifest.json"}}; }
    void setStremioAddons(const std::vector<std::string>&, const std::string&) {}
};
