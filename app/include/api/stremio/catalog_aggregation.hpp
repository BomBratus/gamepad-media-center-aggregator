#pragma once
#include <set>
#include <string>
#include <vector>

namespace stremio {
// Visit a row from each available category before a second row from any of
// them. First occurrence wins for a route shared by synthetic anime/series.
// Selection is pure: callers apply their request/result budgets afterwards.
template <typename T, typename Key>
std::vector<T> interleaveCatalogs(const std::vector<std::vector<T>>& groups, Key key) {
    std::vector<T> out;
    std::set<std::string> seen;
    for (size_t row = 0;; ++row) {
        bool any = false;
        for (const auto& group : groups) {
            if (row >= group.size()) continue;
            any = true;
            if (seen.insert(key(group[row])).second) out.push_back(group[row]);
        }
        if (!any) return out;
    }
}
} // namespace stremio
