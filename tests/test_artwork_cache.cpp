#include "utils/artwork_cache.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
int main() {
    auto dir = std::filesystem::temp_directory_path() / ("gmca-artwork-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        ArtworkCache cache(dir.string(), 10);
        assert(cache.read("a").empty());
        cache.store("a", "123456");
        assert(cache.read("a") == "123456");
        cache.store("too-big", "12345678901");
        assert(cache.read("too-big").empty());
    }
    {
        ArtworkCache reopened(dir.string(), 10);
        assert(reopened.read("a") == "123456");
        reopened.store("b", "abcdef");
        assert(reopened.read("a").empty());
        assert(reopened.read("b") == "abcdef");
        reopened.store("b", "ignored");
        assert(reopened.read("b") == "abcdef");
    }
    size_t bytes = 0;
    for (const auto& file : std::filesystem::directory_iterator(dir)) { bytes += file.file_size(); assert(file.path().extension() != ".part"); }
    assert(bytes <= 10);
    std::filesystem::remove_all(dir);
    std::cout << "Artwork reuse across restarts and bounded eviction passed\n";
}
