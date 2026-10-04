#pragma once
#include "utils/task_queue.hpp"
#include <functional>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

// Small fixed worker owner. Blocking orchestrators must not occupy the workers
// executing the HTTP fan-out they wait for. Also used serially for transfers.
class Executor {
public:
    using Task = std::function<void()>;
    explicit Executor(size_t count = 1) {
        try {
            for (size_t i = 0; i < count; ++i) workers.emplace_back([this] {
                Task task;
                while (queue.take(task)) {
                    try { task(); } catch (...) { /* boundary owns reporting */ }
                    task = {}; // release captured state before waiting
                }
            });
        } catch (...) { stop(); throw; }
    }
    ~Executor() { stop(); }
    void submit(Task task, TaskPriority priority = TaskPriority::Normal) {
        if (!queue.submit(priority, std::move(task)))
            throw std::runtime_error("Executor stopped or queue full");
    }
    void stop() {
        std::lock_guard<std::mutex> guard(stopMutex);
        queue.stop();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        workers.clear();
    }
private:
    TaskQueue<Task> queue;
    std::mutex stopMutex;
    std::vector<std::thread> workers;
};

inline Executor& stremioOperations() {
    // Two bounded callers; the HTTP pool remains free to complete their waits.
    static Executor operations(2);
    return operations;
}
