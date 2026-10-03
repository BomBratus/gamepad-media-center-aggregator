#include "api/stremio/archive_playback_gate.hpp"
#include <cassert>
#include <future>
#include <iostream>
using namespace stremio::archive;
int main() {
    PlaybackGate gate;
    auto first = gate.start();
    auto second = gate.start();
    assert(first && second);
    gate.pause();
    assert(first->load() && second->load());
    assert(!gate.start());
    auto waiter = std::async(std::launch::async, [&] { gate.wait(); });
    assert(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    gate.finish(first);
    assert(waiter.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    gate.finish(second);
    assert(waiter.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    gate.pause(); // overlapping player lifetimes during navigation
    assert(!gate.resume());
    assert(!gate.start());
    assert(gate.resume());
    auto resumed = gate.start();
    assert(resumed && !resumed->load());
    gate.finish(resumed);
    gate.pause();
    assert(!gate.resume()); // no background pass to restart
    gate.pause();
    assert(!gate.start()); // a scheduled refresh during playback is deferred
    assert(gate.resume());
    std::cout << "archive playback gate tests passed\n";
}
