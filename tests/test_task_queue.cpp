#include "utils/task_queue.hpp"
#include <cassert>
#include <future>
#include <iostream>

int main() {
    TaskQueue<int> queue(64);
    assert(queue.submit(TaskPriority::Background, 42));
    for (int i = 0; i < 32; ++i) assert(queue.submit(TaskPriority::Interactive, i));
    int value;
    assert(queue.take(value) && value == 0); // queued interactive overtakes background
    bool background = false;
    for (int i = 0; i < 11; ++i) {
        assert(queue.take(value));
        if (value == 42) { background = true; break; }
    }
    assert(background);
    queue.stop();
    assert(!queue.submit(TaskPriority::Interactive, 99));
    while (queue.take(value)) {}
    TaskQueue<int> idle;
    auto worker = std::async(std::launch::async, [&] { int task; return idle.take(task); });
    idle.stop();
    assert(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready && !worker.get());
    idle.stop(); // idempotent
    TaskQueue<int> bounded(1);
    assert(bounded.submit(TaskPriority::Normal, 1));
    assert(!bounded.submit(TaskPriority::Normal, 2));
    bounded.stop();
    assert(bounded.take(value) && value == 1); // drain accepted tasks
    assert(!bounded.take(value));
    std::cout << "task queue tests passed\n";
}
