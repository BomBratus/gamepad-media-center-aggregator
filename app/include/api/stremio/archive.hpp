#pragma once

#include "api/stremio/archive_model.hpp"
#include "api/stremio/archive_cursor.hpp"
#include "api/stremio/archive_playback_gate.hpp"
#include "api/stremio/archive_query_queue.hpp"
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
    Cursor cursor;
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
    uint64_t beginPlayback(); // UI thread, parks the staging writer
    void waitForPlayback(); // stream worker, waits for cooperative suspension
    void playbackState(uint64_t session, bool healthy);
    void endPlayback(uint64_t session); // after the video view is destroyed
    void query(const Filter& filter, const Cursor& cursor, size_t limit, bool random,
        std::function<void(Result)> callback, std::shared_ptr<const Snapshot> snapshot = {});
private:
    Cache();
    ~Cache();
    void shutdown();
    struct State;
    std::shared_ptr<State> current();
    std::shared_ptr<State> state;
    PlaybackGate playbackGate;
    PlaybackGate::Mode playbackMode = PlaybackGate::Mode::Foreground;
    QueryQueue queries;
    QueryQueue builds; // never parks an addon/network worker
    bool exiting = false;
};

} // namespace stremio::archive
