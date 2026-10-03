#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace stremio::archive {

// Local SQLite work must never wait behind Borealis's addon/network queue.
// One reader worker also keeps queries on a published connection ordered.
class QueryQueue {
public:
    QueryQueue() : worker([this] { run(); }) {}
    ~QueryQueue() { stop(); }
    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            tasks.push_back(std::move(task));
        }
        changed.notify_one();
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task(); // callers contain errors and always deliver their result
        }
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread worker;
};

} // namespace stremio::archive
