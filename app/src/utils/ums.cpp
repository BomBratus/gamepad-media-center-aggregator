#include "utils/ums.hpp"

#ifdef __PS4__
int Ums::init() {
    this->devices.push_back(Device{.id = -1, .name = "HardDisk", .mount = "/data"});
    return 0;
}
#else
#include <cstdlib>

int Ums::init() {
    const char* home = std::getenv("HOME");
    if (home && *home) this->devices.push_back(Device{.id = -1, .name = "Home", .mount = home});
    return 0;
}
#endif

bool Ums::unmount(const Device& dev) { return false; }
