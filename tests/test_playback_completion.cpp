#include "api/stremio/playback_completion.hpp"
#include <cassert>
#include <cstdio>
int main() {
    assert(!stremio::completedPosition(899, 1000));
    assert(stremio::completedPosition(900, 1000));
    assert(!stremio::completedPosition(1000, 0));
    stremio::PlaybackCompletion completion;
    completion.begin("account\nepisode", "session1");
    assert(completion.accept("account\nepisode", false));
    assert(completion.canSave("account\nepisode"));
    assert(completion.accept("account\nepisode", true));
    assert(!completion.canSave("account\nepisode"));
    assert(!completion.accept("account\nepisode", true)); // EOF after threshold
    assert(!completion.accept("account\nepisode", false)); // pause/seek cannot revive
    completion.begin("account\nepisode", "session1"); // same-session source recovery
    assert(!completion.canSave("account\nepisode"));
    completion.begin("account\nepisode", "session2"); // explicit replay
    assert(completion.canSave("account\nepisode"));
    assert(completion.accept("account\nepisode", false));
    completion.begin("account\nepisode2", "session2");
    assert(completion.canSave("account\nepisode2"));
    assert(completion.accept("account\nepisode2", true));
    assert(completion.canSave("other account\nepisode2"));
    completion.unwatch("account\nepisode2");
    assert(completion.canSave("account\nepisode2"));
    assert(completion.accept("account\nepisode2", true));
    std::puts("playback completion: PASS");
}
