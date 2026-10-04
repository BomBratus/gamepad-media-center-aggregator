#include "utils/executor.hpp"
#include <cassert>
#include <atomic>
#include <iostream>

int main() {
    Executor executor;
    std::atomic<int> sequence{0};
    for (int i = 0; i < 20; ++i) executor.submit([&, i] { assert(sequence++ == i); });
    executor.stop();
    assert(sequence == 20);
    bool rejected = false;
    try { executor.submit([] {}); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    executor.stop();
    // A blocked transfer never occupies a network orchestration worker.
    Executor transfers, network(2);
    std::promise<void> release, started, interactive;
    auto gate = release.get_future();
    transfers.submit([&] { started.set_value(); gate.wait(); });
    started.get_future().wait();
    network.submit([&] { interactive.set_value(); }, TaskPriority::Interactive);
    assert(interactive.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    release.set_value();
    transfers.stop(); network.stop();
    std::cout << "executor tests passed\n";
}
