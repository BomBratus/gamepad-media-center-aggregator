#pragma once

#include <string>

namespace ps4diag {

/// Initialize the PS4 renderer diagnostic log. On the PS4 Stremio-only build
/// the previous session is preserved as ps4-render-debug.previous.log.
/// Other platforms compile these helpers as no-ops.
void init(const std::string& configDir, const std::string& version, const std::string& commit);

/// Append one sanitized diagnostic event. Callers must never pass media URLs,
/// account tokens, subtitle text, IP addresses or other user data.
void write(const std::string& line);

}  // namespace ps4diag
