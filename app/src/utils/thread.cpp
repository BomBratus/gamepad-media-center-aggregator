#include <borealis/core/logger.hpp>
#include <fmt/format.h>
#include "utils/thread.hpp"
#include <algorithm>
#include "utils/background_governor.hpp"
#include <stdexcept>
#include "api/http.hpp"

#ifdef BOREALIS_USE_STD_THREAD
size_t ThreadPool::max_thread_num = std::min<size_t>(4, std::max(1u, std::thread::hardware_concurrency()));
#else
size_t ThreadPool::max_thread_num = 4;
#endif

ThreadPool::ThreadPool() {
    this->start(max_thread_num > 0 ? max_thread_num : 1);
}

ThreadPool::~ThreadPool() { stop(); }

void ThreadPool::start(size_t num) {
    std::lock_guard<std::mutex> locker(this->threadMutex);
    if (isStop) throw std::runtime_error("Cannot restart stopped ThreadPool");
    num = std::clamp<size_t>(num, 1, 4);
    while (this->threads.size() < num) {
#ifdef BOREALIS_USE_STD_THREAD
        Thread th = std::make_shared<std::thread>(task_loop, this);
#else
        Thread th = 0;
        if (pthread_create(&th, nullptr, task_loop, this) != 0)
            throw std::runtime_error("Cannot create ThreadPool worker");
#endif
        this->threads.push_back(th);
    }
    brls::Logger::info("ThreadPool start {}", this->threads.size());
}

void *ThreadPool::task_loop(void *ptr) {
    ThreadPool *p = reinterpret_cast<ThreadPool *>(ptr);
    HTTP s;
    Task task;
    while (p->tasks.take(task, [](TaskPriority priority) {
        return priority != TaskPriority::Background || gmca::backgroundGovernor().backgroundAllowed();
    })) {
        if (task) {
            try {
                task(s);
            } catch (const std::exception &ex) {
                brls::Logger::error("error: pool task {}", ex.what());
            }
        }
        task = {}; // release captured image/group state before the idle wait
    }

    brls::Logger::verbose("thread: exit {}", fmt::ptr(p));
    return nullptr;
}

void ThreadPool::stop() {
    std::lock_guard<std::mutex> stopLock(stopMutex);
    std::list<Thread> joining;
    {
        std::lock_guard<std::mutex> locker(threadMutex);
        if (isStop) return;
        isStop = true;
        tasks.stop();
        joining.swap(threads);
    }
    // Worker code can inspect size without waiting behind its own join.
    for (auto& th : joining) {
#ifdef BOREALIS_USE_STD_THREAD
        th->join();
#else
        pthread_join(th, nullptr);
#endif
    }
}
