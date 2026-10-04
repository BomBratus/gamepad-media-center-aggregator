#pragma once

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

// Application-level QoS; never changes OS thread priorities. FIFO within each
// class, weighted service across classes, bounded queued work, drain on stop.
enum class TaskPriority { Interactive, Normal, Background };

template <typename Task> class TaskQueue {
public:
    explicit TaskQueue(size_t capacity = 256) : capacity(capacity) {}
    bool submit(TaskPriority priority, Task task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopped || pending == capacity) return false;
            tasks[static_cast<size_t>(priority)].push_back(std::move(task));
            ++pending;
        }
        ready.notify_one();
        return true;
    }
    bool take(Task& task) {
        std::unique_lock<std::mutex> lock(mutex);
        ready.wait(lock, [this] { return stopped || pending; });
        if (!pending) return false;
        // Even with continuously arriving interactive work, each nonempty
        // lower class receives service within eleven dispatches.
        constexpr std::array<size_t, 11> order{{0,0,0,0,0,0,0,0,1,1,2}};
        for (size_t i = 0; i < order.size(); ++i) {
            auto& queue = tasks[order[cursor]];
            cursor = (cursor + 1) % order.size();
            if (queue.empty()) continue;
            task = std::move(queue.front());
            queue.pop_front();
            --pending;
            return true;
        }
        return false;
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }
        ready.notify_all();
    }
private:
    const size_t capacity;
    std::mutex mutex;
    std::condition_variable ready;
    std::array<std::deque<Task>, 3> tasks;
    size_t pending = 0, cursor = 0;
    bool stopped = false;
};
