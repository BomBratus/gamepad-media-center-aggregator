#include "api/stremio/archive_query_queue.hpp"
#include <cassert>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    // Model a stuck addon task on the unrelated network queue. Archive must
    // finish before it is released, and preserve its own regular query order.
    std::promise<void> releaseAddon, addonStarted, archiveFinished;
    auto release = releaseAddon.get_future();
    std::thread addon([&] { addonStarted.set_value(); release.wait(); });
    addonStarted.get_future().wait();
    std::vector<int> pages;
    stremio::archive::QueryQueue queries;
    for (int i = 0; i < 3; ++i) queries.submit([&, i] { pages.push_back(i); });
    queries.submit([&] { archiveFinished.set_value(); });
    assert(archiveFinished.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(pages == std::vector<int>({0, 1, 2}));

    // Interactive work is latest-wins. Once request 1 is running, request 3
    // must cancel it and replace request 2 before request 2 ever starts.
    std::promise<void> firstStarted, firstCancelled, latestFinished;
    std::mutex seenMutex;
    std::vector<int> seen;
    queries.submitLatest([&](const stremio::archive::QueryQueue::Cancel& cancel) {
        {
            std::lock_guard<std::mutex> lock(seenMutex);
            seen.push_back(1);
        }
        firstStarted.set_value();
        while (!cancel->load()) std::this_thread::yield();
        firstCancelled.set_value();
    });
    firstStarted.get_future().wait();
    queries.submitLatest([&](const stremio::archive::QueryQueue::Cancel&) {
        std::lock_guard<std::mutex> lock(seenMutex);
        seen.push_back(2);
    });
    queries.submitLatest([&](const stremio::archive::QueryQueue::Cancel&) {
        {
            std::lock_guard<std::mutex> lock(seenMutex);
            seen.push_back(3);
        }
        latestFinished.set_value();
    });
    assert(firstCancelled.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    assert(latestFinished.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    {
        std::lock_guard<std::mutex> lock(seenMutex);
        assert(seen == std::vector<int>({1, 3}));
    }

    queries.stop();
    queries.stop(); // explicit shutdown plus destructor is safe
    releaseAddon.set_value();
    addon.join();
}
