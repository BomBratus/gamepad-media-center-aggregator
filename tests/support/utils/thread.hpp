#pragma once
#include "utils/executor.hpp"
#include "api/http.hpp"
class ThreadPool {
public:
    using Task = std::function<void(HTTP&)>;
    inline static size_t max_thread_num = 2;
    static ThreadPool& instance() { static ThreadPool pool; return pool; }
    bool trySubmit(TaskPriority priority, Task task) {
        try {
            executor.submit([task = std::move(task)] { HTTP http; task(http); }, priority);
            return true;
        } catch (...) { return false; }
    }
    void stop() { executor.stop(); }
private:
    Executor executor{2};
};
