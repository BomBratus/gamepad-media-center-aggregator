#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace stremio::archive {

// Local SQLite work must never wait behind Borealis's addon/network queue.
// One reader worker keeps queries on a published connection ordered. Interactive
// result requests use latest-wins semantics: a newer filter/page request cancels
// the running supersedable query and replaces any supersedable work still queued.
class QueryQueue {
public:
    using Cancel = std::shared_ptr<std::atomic_bool>;

    QueryQueue() : worker([this] { run(); }) {}
    ~QueryQueue() { stop(); }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            tasks.push_back({std::move(task), {}});
        }
        changed.notify_one();
    }

    void submitLatest(std::function<void(const Cancel&)> task) {
        auto cancel = std::make_shared<std::atomic_bool>(false);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;

            // The running SQLite statement observes this through
            // sqlite3_progress_handler. Pending supersedable tasks keep their
            // callback/lifetime cleanup, but are marked cancelled so they skip
            // SQLite work when eventually drained.
            if (activeCancel) activeCancel->store(true);
            for (auto& pending : tasks)
                if (pending.cancel) pending.cancel->store(true);

            // Status polling is cheap and does not need to delay an interactive
            // filter change that arrived later. Put the newest request first;
            // cancelled older requests drain afterward without doing SQL.
            tasks.push_front({[task = std::move(task), cancel] { task(cancel); }, cancel});
        }
        changed.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) return;
            stopping = true;
            if (activeCancel) activeCancel->store(true);
            for (auto& task : tasks)
                if (task.cancel) task.cancel->store(true);
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }

private:
    struct Task {
        std::function<void()> run;
        Cancel cancel;
    };

    void run() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || !tasks.empty(); });
                if (tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop_front();
                activeCancel = task.cancel;
            }

            task.run(); // callers contain errors and always deliver their result

            {
                std::lock_guard<std::mutex> lock(mutex);
                if (activeCancel == task.cancel) activeCancel.reset();
            }
        }
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Task> tasks;
    Cancel activeCancel;
    bool stopping = false;
    std::thread worker;
};

} // namespace stremio::archive
