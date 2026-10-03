#include "api/stremio/archive_query_queue.hpp"
#include <cassert>
#include <future>
#include <vector>

int main() {
    // Model a stuck addon task on the unrelated network queue. Archive must
    // finish before it is released, and preserve its own query/paging order.
    std::promise<void> releaseAddon, addonStarted, archiveFinished;
    auto release = releaseAddon.get_future();
    std::thread addon([&] { addonStarted.set_value(); release.wait(); });
    addonStarted.get_future().wait();
    std::vector<int> pages;
    stremio::archive::QueryQueue queries;
    for (int i = 0; i < 3; ++i) queries.submit([&, i] { pages.push_back(i); });
    queries.submit([&] { archiveFinished.set_value(); });
    assert(archiveFinished.get_future().wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    queries.stop();
    assert(pages == std::vector<int>({0, 1, 2}));
    queries.stop(); // explicit shutdown plus destructor is safe
    releaseAddon.set_value();
    addon.join();
}
