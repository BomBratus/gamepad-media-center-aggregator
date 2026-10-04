#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include "../app/src/api/stremio/addons.cpp"

static std::atomic<int> collections{0}, manifests{0};
static std::atomic<bool> failCollection{false};
namespace stremio {
std::vector<std::string> fetchAddonCollection(const std::string&) {
    ++collections;
    if (failCollection) throw std::runtime_error("offline");
    return {"https://first/manifest.json", "https://second/manifest.json"};
}
}
int main() {
    stremio::AddonEngine engine;
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    stremio::requests::transport = [&](const std::string& url) {
        if (++manifests == 1) { entered.set_value(); gate.wait(); }
        return std::string(R"({"id":")") + url + R"(","name":"addon","version":"1","resources":["meta"],"types":["movie"]})";
    };
    auto first = std::async(std::launch::async, [&] { engine.ensureLoaded(); });
    entered.get_future().wait();
    // Routing reads remain responsive while manifests wait on the network.
    auto ui = std::async(std::launch::async, [&] { return engine.browsableTypes(); });
    assert(ui.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    std::vector<std::future<void>> callers;
    for (int i = 0; i < 8; ++i)
        callers.push_back(std::async(std::launch::async, [&] { engine.ensureLoaded(); }));
    release.set_value(); first.get();
    for (auto& caller : callers) caller.get();
    assert(collections == 1 && manifests == 2);
    auto addons = engine.addonsFor("meta", "movie");
    assert(addons.size() == 2 && addons[0].base == "https://first" && addons[1].base == "https://second");
    failCollection = true;
    engine.invalidate(); engine.ensureLoaded();
    addons = engine.addonsFor("meta", "movie");
    assert(addons.size() == 1 && addons[0].base == "https://persisted");
    std::cout << "addon loading tests passed\n";
}
