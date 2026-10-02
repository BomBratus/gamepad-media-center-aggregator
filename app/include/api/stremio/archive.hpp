#pragma once

#include "api/stremio/archive_model.hpp"
#include <functional>
#include <memory>

namespace stremio::archive {

struct Options {
    std::vector<std::string> genres, countries, addons, services;
    bool hasViews = false;
};
struct Result {
    std::vector<media::Item> items;
    Options options;
    size_t total = 0, indexed = 0;
    int64_t refreshed = 0;
    bool refreshing = false, partial = false;
    std::string error;
};

// Capture account/config on the UI thread, then do disk, network and queries
// off-thread. A job owns its account snapshot even if the user switches profiles.
class Cache {
public:
    static Cache& instance();
    void refresh(bool force = false);
    void query(const Filter& filter, size_t offset, size_t limit, bool random,
        std::function<void(Result)> callback);
private:
    Cache();
    struct State;
    std::shared_ptr<State> current();
    std::shared_ptr<State> state;
};

} // namespace stremio::archive
