#pragma once
#include <string>
#include <chrono>
#include <SDL2/SDL.h>
#include <borealis/core/event.hpp>

namespace gmca::test {
// UI and SDL calls run exclusively on the main loop; no worker owns view pointers.
class Harness {
public:
    Harness();
    ~Harness();
    void tick();
private:
    void cleanup();
    brls::Event<>::Subscription exitSubscription_{};
    int socket_ = -1;
    int client_ = -1;
    int device_ = -1;
    SDL_Joystick* joystick_ = nullptr;
    std::string path_;
    std::chrono::steady_clock::time_point clientSince_;
};
std::string requestUrl(const std::string& url, bool write);
}
