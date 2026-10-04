#include "utils/request_context.hpp"
#include "utils/executor.hpp"
#include <cassert>
#include <iostream>
int main() {
    gmca::LatestRequest requests;
    auto old = requests.next();
    { gmca::RequestBinding bind(old); assert(gmca::currentRequest == old); }
    assert(!gmca::currentRequest);
    auto current = requests.next();
    assert(gmca::cancelled(old) && !gmca::cancelled(current));
    Executor executor;
    std::promise<void> started, release;
    auto gate = release.get_future();
    executor.submit([&] { started.set_value(); gate.wait(); });
    started.get_future().wait();
    int callbacks = 0;
    executor.submit([&] { if (!gmca::cancelled(current)) ++callbacks; });
    requests.cancel(); release.set_value(); executor.stop();
    assert(callbacks == 0);
    std::cout << "request cancellation tests passed\n";
}
