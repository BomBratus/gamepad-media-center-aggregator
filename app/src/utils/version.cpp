#include <borealis.hpp>
#if defined(GMCA_LINUX_TEST_BENCH)
#include <unistd.h>
#endif
#include "utils/config.hpp"
#include "utils/dialog.hpp"
#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
#include "utils/ps4_update.hpp"
#endif

using namespace brls::literals;

#define STR_IMPL(x) #x
#define STR(x) STR_IMPL(x)

std::string AppVersion::getVersion() { return STR(APP_VERSION); }

std::string AppVersion::getUpdateVersion() {
#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
    return ps4update::currentVersion();
#else
    return getVersion();
#endif
}

std::string AppVersion::getPackageName() { return STR(BUILD_PACKAGE_NAME); }

std::string AppVersion::getCommit() { return STR(BUILD_TAG_SHORT); }

std::string AppVersion::getPlatform() {
#ifdef __PS4__
    return "PS4";
#elif defined(GMCA_LINUX_TEST_BENCH)
    return "Linux test bench";
#else
#error "GMCA requires PS4 or GMCA_LINUX_TEST_BENCH"
#endif
}

std::string AppVersion::getDeviceName() {
#ifdef __PS4__
    return "PS4";
#elif defined(GMCA_LINUX_TEST_BENCH)
    char name[256] = {};
    if (!gethostname(name, sizeof(name))) return name;
#endif
    return fmt::format("{} for {}", getPackageName(), getPlatform());
}

void AppVersion::checkUpdate(int delay, bool showUpToDateDialog) {
#if defined(__PS4__) && defined(GMCA_PS4_SAFE_SOURCES)
    ps4update::checkUpdate(delay, showUpToDateDialog);
#elif defined(GMCA_LINUX_TEST_BENCH)
    (void)delay;
    if (showUpToDateDialog) Dialog::show("Updates are available on PS4.");
#endif
}
