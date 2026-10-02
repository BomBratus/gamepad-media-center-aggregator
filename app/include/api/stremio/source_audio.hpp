#pragma once

#include <string>
#include <vector>

namespace stremio {
// A release-language hint, not a subtitle-language hint. Keep flag tokens in
// sequence so a subtitle marker can qualify a flag just like it qualifies ITA.
inline bool hasItalianAudio(std::string text) {
    const std::string flag = u8"🇮🇹";
    for (size_t at = 0; (at = text.find(flag, at)) != std::string::npos; at += 8)
        text.replace(at, flag.size(), " itflag ");
    std::vector<std::string> tokens;
    std::string token;
    auto flush = [&] { if (!token.empty()) { tokens.push_back(token); token.clear(); } };
    for (unsigned char c : text) {
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) token += char(c);
        else flush();
    }
    flush();
    auto subtitle = [](const std::string& t) {
        return t == "sub" || t == "subs" || t == "subtitle" || t == "subtitles" ||
               t == "subbed" || t == "sottotitoli";
    };
    auto audio = [](const std::string& t) {
        return t == "audio" || t == "dub" || t == "dubbed" || t == "dual" || t == "multi";
    };
    bool hasSubtitle = false;
    for (const auto& t : tokens) hasSubtitle = hasSubtitle || subtitle(t);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const bool explicitAudio = (i > 0 && audio(tokens[i-1])) ||
                                   (i+1 < tokens.size() && audio(tokens[i+1]));
        const bool sub = (i > 0 && subtitle(tokens[i-1])) ||
                         (i+1 < tokens.size() && subtitle(tokens[i+1]));
        if (sub) continue;
        if (tokens[i] == "ita") return true;
        if (tokens[i] == "itflag" && (!hasSubtitle || explicitAudio)) return true;
        if ((tokens[i] == "italian" || tokens[i] == "italiano" || tokens[i] == "italiana") &&
            explicitAudio) return true;
    }
    return false;
}
} // namespace stremio
