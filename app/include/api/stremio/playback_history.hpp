#pragma once

#include <api/media/types.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace stremio {

namespace playback_history_detail {

inline std::string sourceUrl(const media::Media& source) {
    if (source.parts.empty()) return {};
    return source.parts.front().key;
}

inline std::string jsonString(const nlohmann::json& value, const char* key) {
    if (!value.is_object()) return {};
    auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : std::string();
}

inline int64_t epochMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline bool isDigit(char value) { return value >= '0' && value <= '9'; }

inline int parseDigits(const std::string& value, size_t start, size_t count) {
    int parsed = 0;
    for (size_t i = start; i < start + count; ++i) parsed = parsed * 10 + (value[i] - '0');
    return parsed;
}

/// Normalize the accepted UTC ISO forms to fixed-width milliseconds so plain
/// string comparison also compares timestamps chronologically.
inline bool normalizeIsoUtc(const std::string& value, std::string& normalized) {
    if (value.size() != 20 && value.size() != 24) return false;
    if (value[4] != '-' || value[7] != '-' || value[10] != 'T' || value[13] != ':' || value[16] != ':')
        return false;
    const bool hasMilliseconds = value.size() == 24;
    if (hasMilliseconds && value[19] != '.') return false;
    if (value.back() != 'Z') return false;

    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16 || i == value.size() - 1 ||
            (hasMilliseconds && i == 19))
            continue;
        if (!isDigit(value[i])) return false;
    }

    const int year = parseDigits(value, 0, 4);
    const int month = parseDigits(value, 5, 2);
    const int day = parseDigits(value, 8, 2);
    const int hour = parseDigits(value, 11, 2);
    const int minute = parseDigits(value, 14, 2);
    const int second = parseDigits(value, 17, 2);
    if (year < 1 || month < 1 || month > 12 || hour > 23 || minute > 59 || second > 59) return false;

    static constexpr int monthLengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    const int daysInMonth = monthLengths[month - 1] + (month == 2 && leapYear ? 1 : 0);
    if (day < 1 || day > daysInMonth) return false;

    normalized = hasMilliseconds ? value : value.substr(0, 19) + ".000Z";
    return true;
}

}  // namespace playback_history_detail

/// Synchronous, account-scoped Stremio playback history stored in one local
/// JSON file. The caller supplies the account scope so this helper has no
/// dependency on application configuration or authentication state.
class PlaybackHistory {
public:
    explicit PlaybackHistory(std::string path) : path_(std::move(path)) { (void)processMutex(); }

    /// Save a playback snapshot. The media ratingKey is the item id in the
    /// JSON document. A position of zero is stored as supplied.
    bool save(const std::string& scope, media::Item item, media::Media source,
        int64_t position, int64_t duration) {
        if (scope.empty() || item.ratingKey.empty() || path_.empty()) return false;

        std::lock_guard<std::mutex> lock(processMutex());
        nlohmann::json root;
        if (!readRoot(root)) return false;
        if (!root.contains(scope) || !root[scope].is_object()) root[scope] = nlohmann::json::object();

        nlohmann::json metadata = {
            {"ratingKey", item.ratingKey},
            {"type", item.type},
            {"title", item.title},
            {"thumb", item.thumb},
            {"guid", item.guid},
        };
        addIfPresent(metadata, "grandparentRatingKey", item.grandparentRatingKey);
        addIfPresent(metadata, "grandparentTitle", item.grandparentTitle);
        addIfPresent(metadata, "grandparentThumb", item.grandparentThumb);
        addIfPresent(metadata, "grandparentArt", item.grandparentArt);

        root[scope][item.ratingKey] = {
            {"position", position},
            {"duration", duration},
            {"item", std::move(metadata)},
            {"source", {
                {"url", playback_history_detail::sourceUrl(source)},
                {"identity", source.sourceIdentity},
            }},
            {"updated", playback_history_detail::epochMilliseconds()},
        };
        return writeRoot(root);
    }

    /// Return a record for one scoped item. Invalid or missing data is an
    /// empty object, so callers can safely use this while recovering a file.
    nlohmann::json load(const std::string& scope, const std::string& id) const {
        if (scope.empty() || id.empty() || path_.empty()) return nlohmann::json::object();

        std::lock_guard<std::mutex> lock(processMutex());
        nlohmann::json root;
        if (!readRoot(root)) return nlohmann::json::object();
        auto scopeIt = root.find(scope);
        if (scopeIt == root.end() || !scopeIt->is_object()) return nlohmann::json::object();
        auto recordIt = scopeIt->find(id);
        if (recordIt == scopeIt->end() || !recordIt->is_object()) return nlohmann::json::object();
        return *recordIt;
    }

    /// Return all valid records for a scope as an id -> record object.
    nlohmann::json records(const std::string& scope) const {
        if (scope.empty() || path_.empty()) return nlohmann::json::object();

        std::lock_guard<std::mutex> lock(processMutex());
        nlohmann::json root;
        if (!readRoot(root)) return nlohmann::json::object();
        auto scopeIt = root.find(scope);
        if (scopeIt == root.end() || !scopeIt->is_object()) return nlohmann::json::object();
        return *scopeIt;
    }

    /// Mark an entry complete while retaining its item and source metadata.
    bool clear(const std::string& scope, const std::string& id) {
        if (scope.empty() || id.empty() || path_.empty()) return false;

        std::lock_guard<std::mutex> lock(processMutex());
        nlohmann::json root;
        if (!readRoot(root)) return false;
        auto scopeIt = root.find(scope);
        if (scopeIt == root.end() || !scopeIt->is_object()) return false;
        auto recordIt = scopeIt->find(id);
        if (recordIt == scopeIt->end() || !recordIt->is_object()) return false;

        (*recordIt)["position"] = 0;
        (*recordIt)["updated"] = playback_history_detail::epochMilliseconds();
        return writeRoot(root);
    }

    /// Format epoch milliseconds as UTC `YYYY-MM-DDTHH:MM:SS.mmmZ`.
    static std::string timestampIso(int64_t epochMs) {
        int64_t seconds = epochMs / 1000;
        int64_t milliseconds = epochMs % 1000;
        if (milliseconds < 0) {
            milliseconds += 1000;
            --seconds;
        }

        const std::time_t time = static_cast<std::time_t>(seconds);
        if (static_cast<int64_t>(time) != seconds) return {};
        std::tm utc{};
#if defined(_WIN32)
        if (::gmtime_s(&utc, &time) != 0) return {};
#else
        if (!::gmtime_r(&time, &utc)) return {};
#endif
        const int year = utc.tm_year + 1900;
        if (year < 0 || year > 9999) return {};

        char formatted[25];
        const int length = std::snprintf(formatted, sizeof(formatted), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
            year, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
            static_cast<long long>(milliseconds));
        return length == 24 ? std::string(formatted, 24) : std::string();
    }

    /// Convert the record's persisted epoch-millisecond timestamp to UTC ISO.
    static std::string updatedIso(const nlohmann::json& record) {
        if (!record.is_object()) return {};
        auto updated = record.find("updated");
        if (updated == record.end() || !updated->is_number_integer()) return {};
        try {
            return timestampIso(updated->get<int64_t>());
        } catch (...) {
            return {};
        }
    }

    /// Prefer local state only when it is genuinely newer than a parseable
    /// remote timestamp. Missing or malformed remote timestamps preserve local
    /// state because recency cannot be established safely.
    static bool localIsNewer(const nlohmann::json& record, const std::string& remoteTimestamp) {
        std::string remote;
        if (!playback_history_detail::normalizeIsoUtc(remoteTimestamp, remote)) return true;
        const std::string local = updatedIso(record);
        if (local.empty()) return false;
        return local > remote;
    }

    /// Match a saved source against the current source list. Exact URLs take
    /// priority; a changed URL can fall back to one unique stable identity.
    static int chooseSource(const nlohmann::json& record, const std::vector<media::Media>& sources) {
        const auto saved = record.find("source");
        if (saved == record.end() || !saved->is_object()) return -1;
        const std::string savedUrl = playback_history_detail::jsonString(*saved, "url");
        const std::string savedIdentity = playback_history_detail::jsonString(*saved, "identity");

        int urlMatch = -1;
        size_t urlMatches = 0;
        if (!savedUrl.empty()) {
            for (size_t i = 0; i < sources.size(); ++i) {
                if (playback_history_detail::sourceUrl(sources[i]) == savedUrl) {
                    urlMatch = static_cast<int>(i);
                    ++urlMatches;
                }
            }
            if (urlMatches == 1) return urlMatch;
        }

        if (savedIdentity.empty()) return -1;
        int identityMatch = -1;
        size_t identityMatches = 0;
        for (size_t i = 0; i < sources.size(); ++i) {
            if (sources[i].playable() && sources[i].sourceIdentity == savedIdentity) {
                identityMatch = static_cast<int>(i);
                ++identityMatches;
            }
        }
        return identityMatches == 1 ? identityMatch : -1;
    }

private:
    static std::mutex& processMutex() {
        // Shared by all instances, making the read-modify-write transaction
        // safe when playback callbacks and UI actions use separate instances.
        static std::mutex mutex;
        return mutex;
    }

    static void addIfPresent(nlohmann::json& object, const char* key, const std::string& value) {
        if (!value.empty()) object[key] = value;
    }

    bool readRoot(nlohmann::json& root) const {
        FILE* file = std::fopen(path_.c_str(), "rb");
        if (!file) {
            if (errno == ENOENT) {
                root = nlohmann::json::object();
                return true;
            }
            return false;
        }

        std::string contents;
        char buffer[8192];
        bool readOk = true;
        while (true) {
            const size_t count = std::fread(buffer, 1, sizeof(buffer), file);
            contents.append(buffer, count);
            if (count < sizeof(buffer)) {
                if (std::ferror(file)) readOk = false;
                break;
            }
        }
        if (std::fclose(file) != 0) readOk = false;
        if (!readOk) return false;

        try {
            root = nlohmann::json::parse(contents);
            if (!root.is_object()) root = nlohmann::json::object();
        } catch (...) {
            // A malformed history must not crash playback or poison future
            // saves. The next successful save replaces it with valid JSON.
            root = nlohmann::json::object();
        }
        return true;
    }

    bool writeRoot(const nlohmann::json& root) const {
        const std::string contents = root.dump(2);
        std::string temporaryPath = path_ + ".tmp.XXXXXX";
        std::vector<char> templateBuffer(temporaryPath.begin(), temporaryPath.end());
        templateBuffer.push_back('\0');

        const int fd = ::mkstemp(templateBuffer.data());
        if (fd < 0) return false;
        temporaryPath.assign(templateBuffer.data());

        FILE* file = ::fdopen(fd, "wb");
        if (!file) {
            ::close(fd);
            ::unlink(temporaryPath.c_str());
            return false;
        }

        bool ok = std::fwrite(contents.data(), 1, contents.size(), file) == contents.size();
        if (ok && std::fflush(file) != 0) ok = false;
        if (ok && ::fsync(::fileno(file)) != 0) ok = false;
        if (std::fclose(file) != 0) ok = false;
        if (!ok) {
            ::unlink(temporaryPath.c_str());
            return false;
        }

        if (std::rename(temporaryPath.c_str(), path_.c_str()) != 0) {
            ::unlink(temporaryPath.c_str());
            return false;
        }
        return true;
    }

    std::string path_;
};

}  // namespace stremio
