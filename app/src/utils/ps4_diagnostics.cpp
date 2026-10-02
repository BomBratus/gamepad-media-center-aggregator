#include "utils/ps4_diagnostics.hpp"

#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <sys/stat.h>

namespace ps4diag {
namespace {

std::mutex sMutex;
FILE* sFile = nullptr;
bool sInitialized = false;
std::chrono::steady_clock::time_point sStart;

void rawWrite(const std::string& line) {
    if (!sFile) return;
    std::fprintf(sFile, "%s\n", line.c_str());
    // Keep the tail recoverable through FTP even if the app is later killed.
    std::fflush(sFile);
}

}  // namespace

void init(const std::string& configDir, const std::string& version, const std::string& commit) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sInitialized) return;
    sInitialized = true;

    if (mkdir(configDir.c_str(), 0777) != 0 && errno != EEXIST) return;

    const std::string current = configDir + "/ps4-render-debug.log";
    const std::string previous = configDir + "/ps4-render-debug.previous.log";

    // One-session rotation: if a problem survives an app restart, Luna can
    // retrieve both files over GoldHEN FTP without losing the failing session.
    std::remove(previous.c_str());
    std::rename(current.c_str(), previous.c_str());

    sFile = std::fopen(current.c_str(), "w");
    if (!sFile) return;
    std::setvbuf(sFile, nullptr, _IOLBF, 0);
    sStart = std::chrono::steady_clock::now();

    rawWrite("# GMCA PS4 renderer diagnostics");
    rawWrite("# privacy: no URLs, tokens, IPs or subtitle contents are recorded");
    rawWrite("version=" + version + " commit=" + commit);
}

void write(const std::string& line) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (!sFile) return;

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sStart);
    char prefix[48];
    std::snprintf(prefix, sizeof(prefix), "[%010lld ms] ", static_cast<long long>(elapsed.count()));
    rawWrite(std::string(prefix) + line);
}

}  // namespace ps4diag

#else

namespace ps4diag {

void init(const std::string&, const std::string&, const std::string&) {}
void write(const std::string&) {}

}  // namespace ps4diag

#endif
