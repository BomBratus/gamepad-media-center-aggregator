#pragma once

#include "api/media/types.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <set>

namespace stremio::archive {

using Json = nlohmann::json;

inline std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}

inline std::vector<std::string> tags(const Json& j, const char* key) {
    std::vector<std::string> result;
    auto it = j.find(key);
    if (it == j.end()) return result;
    if (it->is_string()) {
        // Country lists are commonly comma-separated strings.
        std::string value = it->get<std::string>();
        size_t start = 0;
        while (start < value.size()) {
            size_t end = value.find(',', start);
            auto tag = value.substr(start, end == std::string::npos ? end : end - start);
            auto first = tag.find_first_not_of(" \t");
            auto last = tag.find_last_not_of(" \t");
            if (first != std::string::npos) result.push_back(tag.substr(first, last - first + 1));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    } else if (it->is_array()) {
        for (const auto& v : *it) if (v.is_string()) result.push_back(v.get<std::string>());
    }
    return result;
}

inline bool contains(const std::vector<std::string>& values, const std::string& wanted) {
    return std::any_of(values.begin(), values.end(), [&](const auto& v) { return lower(v) == lower(wanted); });
}

struct Record {
    Json meta;
    std::vector<std::string> addons;
    int64_t added = 0;
    int64_t updated = 0;
};

inline std::vector<std::string> genres(const Record& r) {
    auto values = tags(r.meta, "genres");
    auto links = r.meta.find("links");
    if (values.empty() && links != r.meta.end() && links->is_array())
        for (const auto& link : *links)
            if (lower(media::jstr(link, "category")) == "genres") values.push_back(media::jstr(link, "name"));
    return values;
}

inline std::vector<std::string> countries(const Record& r) {
    auto values = tags(r.meta, "country");
    if (values.empty()) values = tags(r.meta, "countries");
    return values;
}

inline std::vector<std::string> services(const Record& r) {
    auto values = tags(r.meta, "services");
    if (values.empty()) values = tags(r.meta, "service");
    return values;
}

inline int64_t year(const Record& r) {
    auto text = media::jstr(r.meta, "year", media::jstr(r.meta, "releaseInfo", media::jstr(r.meta, "released")));
    if (text.size() < 4 || !std::all_of(text.begin(), text.begin() + 4, [](char c) { return c >= '0' && c <= '9'; })) return 0;
    return std::stoll(text.substr(0, 4));
}

inline double rating(const Record& r) {
    double n = media::jnum(r.meta, "imdbRating", media::jnum(r.meta, "rating"));
    return std::isfinite(n) && n >= 0 && n <= 10 ? n : 0;
}

inline int64_t releaseDate(const Record& r) {
    auto date = media::jstr(r.meta, "released");
    if (date.size() >= 10 && date[4] == '-' && date[7] == '-') {
        auto digits = [&](size_t start, size_t length) {
            return std::all_of(date.begin() + start, date.begin() + start + length, [](char c) { return c >= '0' && c <= '9'; });
        };
        if (digits(0, 4) && digits(5, 2) && digits(8, 2)) {
            auto month = std::stoll(date.substr(5, 2)), day = std::stoll(date.substr(8, 2));
            if (month >= 1 && month <= 12 && day >= 1 && day <= 31)
                return std::stoll(date.substr(0, 4)) * 10000 + month * 100 + day;
        }
    }
    return year(r) * 10000; // catalog previews sometimes provide only a year
}

enum class Sort { Release, Updated, Added, Rating, Views, Name, Votes };
struct Filter {
    std::string search, type, genre, country, addon, service;
    int64_t yearFrom = 0, yearTo = 0, minViews = 0;
    int64_t minVotes = 0;
    double minRating = 0;
    int other = 0; // 0=all, 1=with poster, 2=with description
    Sort sort = Sort::Release;
    bool descending = true;
};

inline bool matches(const Record& r, const Filter& f) {
    if (!f.type.empty() && media::jstr(r.meta, "type") != f.type) return false;
    if (!f.search.empty() && lower(media::jstr(r.meta, "name")).find(lower(f.search)) == std::string::npos) return false;
    if (!f.genre.empty() && !contains(genres(r), f.genre)) return false;
    if (!f.country.empty() && !contains(countries(r), f.country)) return false;
    if (!f.addon.empty() && !contains(r.addons, f.addon)) return false;
    if (!f.service.empty() && !contains(services(r), f.service)) return false;
    auto y = year(r);
    if ((f.yearFrom && y < f.yearFrom) || (f.yearTo && (!y || y > f.yearTo))) return false;
    if (f.minRating && rating(r) < f.minRating) return false;
    // `views` is public catalog metadata, never the user's watched/viewCount.
    if (f.minViews && media::jint(r.meta, "views") < f.minViews) return false;
    if (f.minVotes && media::jint(r.meta, "votes") < f.minVotes) return false;
    if (f.other == 1 && media::jstr(r.meta, "poster").empty()) return false;
    if (f.other == 2 && media::jstr(r.meta, "description").empty()) return false;
    return true;
}

inline std::vector<size_t> select(const std::vector<Record>& records, const Filter& filter, bool sorted = true) {
    std::vector<size_t> indices;
    for (size_t i = 0; i < records.size(); ++i) if (matches(records[i], filter)) indices.push_back(i);
    if (!sorted) return indices;
    // Decode each sort key once. Reading JSON/year strings inside the sort
    // comparator was the dominant cost on a 50,000-title archive.
    std::vector<long double> numbers;
    std::vector<std::string> names;
    if (filter.sort == Sort::Name) names.resize(records.size());
    else numbers.resize(records.size());
    for (auto i : indices) {
        const auto& record = records[i];
        switch (filter.sort) {
            case Sort::Release: numbers[i] = releaseDate(record); break;
            case Sort::Added: numbers[i] = record.added; break;
            case Sort::Updated: numbers[i] = record.updated; break;
            case Sort::Rating: numbers[i] = rating(record); break;
            case Sort::Views: numbers[i] = media::jint(record.meta, "views"); break;
            case Sort::Votes: numbers[i] = media::jint(record.meta, "votes"); break;
            case Sort::Name: names[i] = lower(media::jstr(record.meta, "name")); break;
        }
    }
    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        int cmp = 0;
        auto compare = [&](const auto& p, const auto& q) { cmp = p < q ? -1 : (q < p ? 1 : 0); };
        if (filter.sort == Sort::Name) compare(names[a], names[b]);
        else compare(numbers[a], numbers[b]);
        return filter.descending ? cmp > 0 : cmp < 0;
    });
    return indices;
}

// Pick uniformly from the full matching set, independent of sorting/pagination.
template <class Engine>
inline size_t randomMatch(const std::vector<size_t>& matches, Engine& engine) {
    if (matches.empty()) return size_t(-1);
    return matches[std::uniform_int_distribution<size_t>(0, matches.size() - 1)(engine)];
}

inline std::string identity(const Json& meta) {
    return media::jstr(meta, "type") + ":" + media::jstr(meta, "id");
}

inline Json serialize(const Record& r) {
    return {{"meta", r.meta}, {"addons", r.addons}, {"added", r.added}, {"updated", r.updated}};
}

inline Record deserialize(Json j) {
    Record r;
    r.meta = std::move(j.at("meta"));
    if (!r.meta.is_object() || media::jstr(r.meta, "id").empty()) throw std::runtime_error("Invalid archive record");
    r.addons = tags(j, "addons");
    r.added = media::jint(j, "added"); r.updated = media::jint(j, "updated");
    return r;
}

} // namespace stremio::archive
