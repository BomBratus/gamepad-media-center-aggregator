#include "utils/serial_writer.hpp"
#include <cassert>
#include <iostream>
int main() {
    SerialWriter writer;
    int sequence = 0;
    for (int i = 0; i < 20; ++i) writer.submit([&, i] { assert(sequence++ == i); });
    writer.flush();
    assert(sequence == 20);
    std::promise<void> entered, release;
    auto gate = release.get_future();
    writer.submit([&] { entered.set_value(); gate.wait(); });
    entered.get_future().wait();
    writer.submit([&] { sequence = 21; }, "config");
    writer.submit([&] { sequence = 22; }, "config");
    writer.submit([&] { assert(sequence == 22); }); // fence coalescing
    writer.submit([&] { sequence = 23; }, "config");
    release.set_value(); writer.stop();
    assert(sequence == 23);
    bool rejected = false;
    try { writer.submit([] {}); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    std::cout << "serial writer tests passed\n";
}
