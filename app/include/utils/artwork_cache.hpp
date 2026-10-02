#pragma once
#include "utils/image_cache.hpp"
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>
#include <map>

// Worker-only, bounded cache for artwork already fetched while browsing.
// Separate from downloaded media's permanent offline artwork.
class ArtworkCache {
public:
    explicit ArtworkCache(std::string directory, size_t budget = 128 * 1024 * 1024)
        : directory(std::move(directory)), budget(budget) {}
    std::string read(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex);
        try {
            std::ifstream file(directory + "/" + ImageCache::key(url), std::ios::binary);
            std::ostringstream data;
            data << file.rdbuf();
            return data.str();
        } catch (...) { return {}; }
    }
    void store(const std::string& url, const std::string& data) {
        if (data.empty() || data.size() > budget) return;
        std::lock_guard<std::mutex> lock(mutex);
        try {
            namespace fs = std::filesystem;
            fs::create_directories(directory);
            if (!loaded) {
                for (const auto& entry : fs::directory_iterator(directory)) {
                    auto name = entry.path().filename().string();
                    if (name.size() != 16 || !entry.is_regular_file()) continue;
                    auto size = entry.file_size();
                    entries[name] = {size, entry.last_write_time()};
                    bytes += size;
                }
                loaded = true;
            }
            auto key = ImageCache::key(url);
            if (entries.count(key)) return;
            while (!entries.empty() && (bytes + data.size() > budget || entries.size() >= 1000)) {
                auto oldest = std::min_element(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                    return a.second.time < b.second.time;
                });
                fs::remove(directory + "/" + oldest->first);
                bytes -= oldest->second.size;
                entries.erase(oldest);
            }
            auto path = directory + "/" + key;
            std::ofstream file(path + ".part", std::ios::binary | std::ios::trunc);
            file.exceptions(std::ios::badbit | std::ios::failbit);
            file.write(data.data(), data.size());
            file.close();
            fs::rename(path + ".part", path);
            entries[key] = {data.size(), fs::last_write_time(path)};
            bytes += data.size();
        } catch (...) {} // artwork caching must never prevent rendering
    }
private:
    struct Entry { size_t size; std::filesystem::file_time_type time; };
    std::string directory;
    size_t budget, bytes = 0;
    bool loaded = false;
    std::map<std::string, Entry> entries;
    std::mutex mutex;
};
