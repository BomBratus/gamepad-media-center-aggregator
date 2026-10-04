#include "api/stremio/archive_playback_gate.hpp"
#include <cassert>
#include <future>
#include <iostream>
#include <thread>
using namespace stremio::archive;
using namespace std::chrono_literals;
int main() {
    PlaybackGate gate(0ms);
    auto job = gate.start();
    gate.checkpoint(job); // actively building
    auto player = gate.beginPlayback();
    assert(!job->load() && gate.mode() == PlaybackGate::Mode::Parked);
    auto waiter = std::async(std::launch::async, [&] { gate.wait(); });
    assert(waiter.wait_for(20ms) == std::future_status::timeout);
    auto worker = std::async(std::launch::async, [&] { gate.checkpoint(job); });
    assert(waiter.wait_for(2s) == std::future_status::ready);
    assert(worker.wait_for(20ms) == std::future_status::timeout);
    gate.playbackState(player, true);
    assert(worker.wait_for(2s) == std::future_status::ready);
    assert(gate.mode() == PlaybackGate::Mode::Background && !job->load());

    // A stale closing view cannot release a newly opening player's lease.
    auto next = gate.beginPlayback();
    gate.endPlayback(player);
    gate.playbackState(player, true);
    worker = std::async(std::launch::async, [&] { gate.checkpoint(job); });
    gate.wait();
    assert(worker.wait_for(20ms) == std::future_status::timeout);
    gate.playbackState(next, true);
    assert(worker.wait_for(2s) == std::future_status::ready);
    const auto downloadStart = PlaybackGate::Clock::now();
    gate.checkpoint(job, 4096);
    assert(PlaybackGate::Clock::now() - downloadStart >= 10ms); // 256 KiB/s budget
    // The CPU budget also yields without any network transfer. Use elapsed work
    // rather than a throughput upper bound, which would be flaky on shared CI.
    const auto cpuStart = PlaybackGate::Clock::now();
    while (PlaybackGate::Clock::now() - cpuStart < 3ms) {}
    gate.checkpoint(job);
    assert(PlaybackGate::Clock::now() - cpuStart >= 25ms);
    // A slow disk call may exceed an entire 40ms window. It must still yield,
    // including after an unrelated notification wakes the condition variable.
    gate.checkpoint(job);
    std::this_thread::sleep_for(45ms);
    worker = std::async(std::launch::async, [&] { gate.checkpoint(job); });
    assert(worker.wait_for(30ms) == std::future_status::timeout);
    gate.playbackState(next, true); // notification must not shorten the rest
    assert(worker.wait_for(30ms) == std::future_status::timeout);
    gate.endPlayback(next); // foreground restores speed without waiting ~855ms
    assert(worker.wait_for(2s) == std::future_status::ready);
    next = gate.beginPlayback();
    gate.playbackState(next, true);
    gate.checkpoint(job);
    gate.playbackState(next, false); // buffering parks without cancelling
    worker = std::async(std::launch::async, [&] { gate.checkpoint(job); });
    gate.wait();
    assert(worker.wait_for(20ms) == std::future_status::timeout);
    gate.endPlayback(next);
    assert(worker.wait_for(2s) == std::future_status::ready);
    assert(gate.mode() == PlaybackGate::Mode::Foreground);
    gate.finish(job);

    // A build queued during opening stays queued/parked without blocking stream
    // resolution; shutdown wakes it and cancels once, without needing a player.
    next = gate.beginPlayback();
    job = gate.start();
    gate.wait();
    worker = std::async(std::launch::async, [&] { gate.checkpoint(job); });
    assert(worker.wait_for(20ms) == std::future_status::timeout);
    gate.stop();
    assert(worker.wait_for(2s) == std::future_status::ready && job->load());
    gate.finish(job);
    assert(!gate.start());

    PlaybackGate downloading(0ms);
    auto downloadPlayer = downloading.beginPlayback();
    auto downloadJob = downloading.start();
    downloading.playbackState(downloadPlayer, true);
    worker = std::async(std::launch::async, [&] { downloading.checkpoint(downloadJob, 1024 * 1024); });
    assert(worker.wait_for(20ms) == std::future_status::timeout);
    downloading.wait();
    downloading.stop(); // interrupts a four-second rate-limit wait immediately
    assert(worker.wait_for(2s) == std::future_status::ready && downloadJob->load());
    downloading.finish(downloadJob);

    PlaybackGate delayed(50ms);
    auto delayedPlayer = delayed.beginPlayback();
    auto delayedJob = delayed.start();
    delayed.playbackState(delayedPlayer, true);
    assert(delayed.mode() == PlaybackGate::Mode::Parked);
    worker = std::async(std::launch::async, [&] { delayed.checkpoint(delayedJob); });
    assert(worker.wait_for(2s) == std::future_status::ready);
    assert(delayed.mode() == PlaybackGate::Mode::Background);
    delayed.finish(delayedJob);
    std::cout << "archive playback gate tests passed\n";
}
