#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <list>
#include <atomic>
#include <memory>
#include <stdexcept>
#include "utils/task_queue.hpp"
#ifdef BOREALIS_USE_STD_THREAD
#include <thread>
#else
#include <pthread.h>
#endif
#include <borealis/core/singleton.hpp>
#include "api/http.hpp"

class ThreadPool : public brls::Singleton<ThreadPool> {
public:
    using Task = std::function<void(HTTP& s)>;

    explicit ThreadPool();
    virtual ~ThreadPool();

    bool trySubmit(TaskPriority priority, Task fn) {
        return tasks.submit(priority, std::move(fn));
    }
    void submit(TaskPriority priority, Task fn) {
        if (!trySubmit(priority, std::move(fn)))
            throw std::runtime_error("ThreadPool stopped or queue full");
    }
    void submit(Task fn) { submit(TaskPriority::Normal, std::move(fn)); }

    /// @brief 创建线程
    void start(size_t num);

    size_t size() const { std::lock_guard<std::mutex> lock(threadMutex); return threads.size(); }

    /// @brief 停止所有线程
    void stop();

    static size_t max_thread_num;

private:
    static void* task_loop(void*);

#ifdef BOREALIS_USE_STD_THREAD
    typedef std::shared_ptr<std::thread> Thread;
#else
    typedef pthread_t Thread;
#endif

    std::list<Thread> threads;
    mutable std::mutex threadMutex;
    std::mutex stopMutex;
    TaskQueue<Task> tasks;
    bool isStop = false;
};