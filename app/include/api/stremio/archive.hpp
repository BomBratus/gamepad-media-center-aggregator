#pragma once

#include "api/stremio/archive_model.hpp"
#include "api/stremio/archive_playback_gate.hpp"
#include <functional>
#include <memory>

namespace stremio::archive {

struct Snapshot;

struct Options {
    std::vector<std::string> genres, countries, addons, services;
    bool hasViews = false;
    bool hasVotes = false;
};
struct Result {
    std::vector<media::Item> items;
    std::shared_ptr<const Snapshot> snapshot;
    Options options;
    size_t total = 0, indexed = 0;
    int64_t refreshed = 0;
    bool refreshing = false, partial = false;
    std::string error;
};

// Capture the shared index path on the UI thread; disk, network and queries run
// off-thread. Public IMDb metadata is independent of account/addon changes.
class Cache {
public:
    static Cache& instance();
    void refresh(bool force = false);
    void pauseForPlayback(); // UI thread, cancels the background import/download
    void waitForPlayback();  // worker thread, waits for checkpoint/memory cleanup
    void resumeAfterPlayback(); // UI thread, after the video view is destroyed
    void query(const Filter& filter, size_t offset, size_t limit, bool random,
        std::function<void(Result)> callback, std::shared_ptr<const Snapshot> snapshot = {});
private:
    Cache();
    struct State;
    std::shared_ptr<State> current();
    std::shared_ptr<State> state;
    PlaybackGate playbackGate;
    bool exiting = false;
};

} // namespace stremio::archive
