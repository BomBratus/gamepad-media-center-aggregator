#pragma once
#include "api/media/types.hpp"
#include <algorithm>
#include <cctype>

namespace stremio {
/// Conservative PS4 source classification. The codec/resolution fields are
/// structured values parsed above; HDR/Dolby Vision are inferred only from the
/// original Stremio name/title text and never from an absent token.
struct Ps4SourceSafety {
    bool highResolution = false;
    bool hdr = false;
    bool dolbyVision = false;
    bool hevc = false;
    bool av1 = false;
};

inline Ps4SourceSafety ps4SourceSafety(const media::Media& m) {
    Ps4SourceSafety s;
    s.highResolution = m.videoResolution == "4K" || m.videoResolution == "1440p";
    s.hevc = m.videoCodec == "HEVC";
    s.av1 = m.videoCodec == "AV1";

    std::string text = m.sourceName + " " + m.sourceTitle;
    for (auto& c : text) c = (char)std::tolower((unsigned char)c);
    s.dolbyVision = text.find("dolby vision") != std::string::npos || text.find("dovi") != std::string::npos;
    s.hdr = text.find("hdr") != std::string::npos;
    return s;
}

/// Higher is safer for the PS4. Unknown codec stays Unknown (rank 4): it is not
/// promoted to H.264 just because the addon omitted a codec token.
inline int ps4SafeRank(const media::Media& m) {
    Ps4SourceSafety s = ps4SourceSafety(m);
    if (s.av1) return 0;
    if (s.highResolution) return 1;
    if (s.hdr || s.dolbyVision) return 2;
    if (s.hevc) return 3;
    if (m.videoCodec == "H.264") return 5;
    return 4;
}

inline std::string ps4WarningLabel(const media::Media& m) {
    Ps4SourceSafety s = ps4SourceSafety(m);
    std::string out;
    auto add = [&out](const char* value) {
        if (!out.empty()) out += " / ";
        out += value;
    };
    if (s.av1) add("AV1");
    if (s.highResolution) add(m.videoResolution == "4K" ? "4K" : "1440p");
    if (s.dolbyVision) add("Dolby Vision");
    else if (s.hdr) add("HDR");
    if (s.hevc) add("HEVC");
    return out;
}

// Preserve addon quality/language/seeders ordering inside each compatibility
// group. Only explicit PS4 risk moves behind unknown/safer playable sources;
// retain risky alternatives and put unplayable placeholders last.
inline void orderPs4Sources(std::vector<media::Media>& sources) {
    auto playableEnd = std::stable_partition(sources.begin(), sources.end(),
        [](const media::Media& m) { return m.playable(); });
    std::stable_partition(sources.begin(), playableEnd,
        [](const media::Media& m) { return ps4SafeRank(m) >= 4; });
}
} // namespace stremio
