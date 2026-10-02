#pragma once
#include "api/stremio/archive_model.hpp"
#include <cstdio>
#include <fstream>

namespace stremio::archive {
inline constexpr size_t MAX_RECORDS = 50000;
inline constexpr size_t MAX_CACHE_BYTES = 64 * 1024 * 1024;

struct Snapshot {
    std::vector<Record> records;
    int64_t refreshed = 0;
    bool partial = false;
    Json crawl = Json::object(); // slice key -> next skip and last-page identities
    bool crawlFinished = false;
    int crawlVersion = 0;
};

inline Snapshot readSnapshot(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    auto bytes = file.tellg();
    if (bytes < 0 || static_cast<size_t>(bytes) > MAX_CACHE_BYTES) throw std::runtime_error("Archive cache too large");
    file.seekg(0);
    auto json = Json::parse(file);
    if (media::jint(json, "version") != 1 || !json.at("records").is_array() || json.at("records").size() > MAX_RECORDS)
        throw std::runtime_error("Invalid archive cache");
    Snapshot result;
    result.refreshed = media::jint(json, "refreshed");
    result.partial = media::jbool(json, "partial");
    result.crawlVersion = media::jint(json, "crawlVersion");
    result.crawlFinished = media::jbool(json, "crawlFinished");
    if (json.contains("crawl") && json["crawl"].is_object()) result.crawl = json["crawl"];
    for (const auto& row : json.at("records")) result.records.push_back(deserialize(row));
    return result;
}

// The previous cache remains intact if encoding, writing or rename fails.
inline void writeSnapshot(const std::string& path, const Snapshot& snapshot) {
    Json json = {{"version", 1}, {"refreshed", snapshot.refreshed}, {"partial", snapshot.partial}, {"records", Json::array()},
        {"crawl", snapshot.crawl}, {"crawlFinished", snapshot.crawlFinished}, {"crawlVersion", snapshot.crawlVersion}};
    for (const auto& record : snapshot.records) json["records"].push_back(serialize(record));
    auto bytes = json.dump();
    if (snapshot.records.size() > MAX_RECORDS || bytes.size() > MAX_CACHE_BYTES) throw std::runtime_error("Archive cache too large");
    const auto temporary = path + ".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.write(bytes.data(), bytes.size());
        file.close();
        if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("Cannot save archive cache");
    } catch (...) {
        std::remove(temporary.c_str());
        throw;
    }
}
} // namespace stremio::archive
