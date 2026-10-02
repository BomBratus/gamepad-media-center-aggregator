#pragma once
#include "api/stremio/archive_model.hpp"
#include <cstdio>
#include <fstream>

namespace stremio::archive {

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
    if (bytes < 0) throw std::runtime_error("Cannot read archive cache");
    file.seekg(0);
    auto json = Json::parse(file);
    if (media::jint(json, "version") != 1 || !json.at("records").is_array())
        throw std::runtime_error("Invalid archive cache");
    Snapshot result;
    result.refreshed = media::jint(json, "refreshed");
    result.partial = media::jbool(json, "partial");
    result.crawlVersion = media::jint(json, "crawlVersion");
    result.crawlFinished = media::jbool(json, "crawlFinished");
    if (json.contains("crawl") && json["crawl"].is_object()) result.crawl = json["crawl"];
    result.records.reserve(json.at("records").size());
    for (auto& row : json.at("records")) result.records.push_back(deserialize(std::move(row)));
    return result;
}

// The previous cache remains intact if encoding, writing or rename fails.
inline void writeSnapshot(const std::string& path, const Snapshot& snapshot) {
    Json header = {{"version", 1}, {"refreshed", snapshot.refreshed}, {"partial", snapshot.partial},
        {"crawl", snapshot.crawl}, {"crawlFinished", snapshot.crawlFinished}, {"crawlVersion", snapshot.crawlVersion}};
    const auto temporary = path + ".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::failbit | std::ios::badbit);
        // Serialize records individually instead of making another full JSON
        // array and encoded copy of the entire archive in memory.
        auto prefix = header.dump();
        prefix.pop_back();
        file << prefix << ",\"records\":[";
        bool first = true;
        for (const auto& record : snapshot.records) {
            if (!first) file << ',';
            first = false;
            file << serialize(record).dump();
        }
        file << "]}";
        file.close();
        if (std::rename(temporary.c_str(), path.c_str()) != 0) throw std::runtime_error("Cannot save archive cache");
    } catch (...) {
        std::remove(temporary.c_str());
        throw;
    }
}
} // namespace stremio::archive
