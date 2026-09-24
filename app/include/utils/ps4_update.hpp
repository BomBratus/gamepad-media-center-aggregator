#pragma once

#ifdef __PS4__

#include <string>

namespace ps4update {

/// PS4 Stremio-only package revision compiled into this build (MM.NN).
std::string currentVersion();

/// Check the dedicated rolling PS4 update feed and, when requested, download,
/// verify and queue the PKG for installation. The delay mirrors AppVersion::checkUpdate.
void checkUpdate(int delay = 2000, bool showUpToDateDialog = false);

}  // namespace ps4update

#endif
