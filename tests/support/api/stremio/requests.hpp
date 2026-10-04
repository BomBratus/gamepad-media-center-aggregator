#pragma once

#include <string>
#include <vector>
#include <functional>

// No catalog/parser test invokes transport. These declarations satisfy the
// inline helpers in api/stremio/types.hpp without the request worker pool.
namespace stremio {
namespace requests {

inline std::string getCached(const std::string&, long, long) { return {}; }
inline std::function<std::string(const std::string&)> transport;
inline std::string get(const std::string& url, long) { return transport ? transport(url) : std::string{}; }
inline void registerBatch(const std::vector<std::string>&, long = 3000, bool = false) {}
inline void registerSearchBatch(const std::vector<std::string>&) {}
inline void clear() {}

}  // namespace requests
}  // namespace stremio
