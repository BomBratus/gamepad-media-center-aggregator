#include "../app/include/api/stremio/requests.hpp"
#include <cassert>
#include <future>
#include <iostream>
int main() {
    gmca::LatestRequest lifetime;
    auto token = lifetime.next();
    std::atomic<int> calls{0};
    HTTP::transport = [&](const std::string&, const HTTP::Cancel& cancel) {
        ++calls;
        assert(cancel == token->cancel);
        while (!cancel->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        throw std::runtime_error("curl aborted");
        return std::string{};
    };
    auto request = std::async(std::launch::async, [&] {
        gmca::RequestBinding bind(token);
        stremio::requests::registerBatch({"https://one", "https://two"});
        try { stremio::requests::get("https://one"); } catch (const std::runtime_error&) { return true; }
        return false;
    });
    while (!calls.load()) std::this_thread::yield();
    lifetime.cancel();
    assert(request.wait_for(std::chrono::seconds(2)) == std::future_status::ready && request.get());
    {
        gmca::RequestBinding bind(token);
        int previous = calls;
        try { stremio::requests::get("https://stale"); assert(false); } catch (const std::runtime_error&) {}
        assert(calls == previous); // stale queued work never starts HTTP
    }
    ThreadPool::instance().stop();
    // Successful metadata cache remains shared across independent view tokens.
    HTTP::transport = [&](const std::string&, const HTTP::Cancel&) { ++calls; return std::string("cached"); };
    auto first = lifetime.next();
    { gmca::RequestBinding bind(first); assert(stremio::requests::getCached("https://meta") == "cached"); }
    int previous = calls;
    auto second = lifetime.next();
    { gmca::RequestBinding bind(second); assert(stremio::requests::getCached("https://meta") == "cached"); }
    assert(calls == previous);
    std::cout << "request HTTP cancellation tests passed\n";
}
