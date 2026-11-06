#pragma once
#include "api/media/types.hpp"
#include <string>

namespace stremio {

/// Stremio addons put the release name on the first line of `title`/`name` and
/// seeders, size and cache markers on the following ones. A picker row shows
/// those separately, so only the first line is a name.
inline std::string sourceFirstLine(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

/// Human-readable source name: the release title the addon supplied, then the
/// stream name, then the addon name. A source that carries any of the three
/// always gets a non-empty name, so a picker row never renders blank.
inline std::string sourceDisplayName(const media::Media& m) {
    for (const std::string* field : {&m.sourceTitle, &m.sourceName, &m.label}) {
        std::string line = sourceFirstLine(*field);
        if (!line.empty()) return line;
    }
    return {};
}

/// One-line row for a source picker: the source name, then the addon and the
/// parsed codec/size when they add information. Dropdown rows are single-line
/// (Borealis ellipsizes them and marquees the focused one), so the parts are
/// joined rather than stacked.
inline std::string sourcePickerLabel(const media::Media& m) {
    std::string out = sourceDisplayName(m);
    auto append = [&out](const std::string& part) {
        if (part.empty() || out.find(part) != std::string::npos) return;
        if (!out.empty()) out += " · ";
        out += part;
    };
    append(m.label);
    append(m.detail);
    return out;
}

} // namespace stremio
