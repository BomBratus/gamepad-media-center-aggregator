#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>

// One FIFO owner of file mutations. Only adjacent snapshots of the same file
// coalesce; barriers/deletes fence snapshots. Callbacks own data and reporting.
class SerialWriter {
public:
    using Task = std::function<void()>;
    SerialWriter() : worker([this] { run(); }) {}
    ~SerialWriter() { stop(); }
    void submit(Task task, std::string snapshotKey = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping) throw std::runtime_error("Persistence writer stopped");
            if (!snapshotKey.empty() && !jobs.empty() && jobs.back().key == snapshotKey)
                jobs.back().task = std::move(task);
            else jobs.push_back({std::move(snapshotKey), std::move(task)});
        }
        changed.notify_one();
    }
    void flush() {
        auto barrier = std::make_shared<std::promise<void>>();
        auto done = barrier->get_future();
        submit([barrier] { barrier->set_value(); });
        done.get();
    }
    void stop() {
        std::lock_guard<std::mutex> joinLock(stopMutex);
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
private:
    struct Job { std::string key; Task task; };
    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (jobs.empty()) return;
                job = std::move(jobs.front()); jobs.pop_front();
            }
            try { job.task(); } catch (...) { /* callback reports its failure */ }
        }
    }
    std::mutex mutex, stopMutex;
    std::condition_variable changed;
    std::deque<Job> jobs;
    bool stopping = false;
    std::thread worker;
};
inline SerialWriter& filePersistence() { static SerialWriter writer; return writer; }
