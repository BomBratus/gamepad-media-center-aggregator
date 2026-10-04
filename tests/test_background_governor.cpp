#include "utils/background_governor.hpp"
#include "utils/task_queue.hpp"
#include <cassert>
#include <future>
#include <iostream>
int main() {
    using Governor = gmca::BackgroundGovernor;
    Governor governor(std::chrono::milliseconds(0));
    assert(governor.state() == Governor::State::Idle);
    governor.begin(1); assert(!governor.backgroundAllowed());
    TaskQueue<int> queue;
    queue.submit(TaskPriority::Background, 1);
    auto worker = std::async(std::launch::async, [&] {
        int result;
        assert(queue.take(result, [&](TaskPriority priority) { return priority != TaskPriority::Background || governor.backgroundAllowed(); }));
        return result;
    });
    // Parked background must not block a subsequently queued interactive job.
    queue.submit(TaskPriority::Interactive, 2);
    assert(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready && worker.get() == 2);
    governor.update(1, true); assert(governor.backgroundAllowed());
    governor.begin(2); assert(!governor.backgroundAllowed());
    governor.update(2, false); assert(!governor.backgroundAllowed());
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto transfer = std::async(std::launch::async, [&] { governor.transfer(cancel, 1024 * 1024); });
    cancel->store(true);
    assert(transfer.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    governor.end(2); assert(governor.state() == Governor::State::Stable);
    governor.end(1); assert(governor.state() == Governor::State::Idle);
    queue.stop(); int result; assert(queue.take(result) && result == 1);
    governor.stop(); assert(governor.state() == Governor::State::Stopped);
    std::cout << "background governor tests passed\n";
}
