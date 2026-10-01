#pragma once

#include <string>

// No catalog/parser test invokes transport. These declarations satisfy the
// inline helpers in api/stremio/types.hpp without the request worker pool.
namespace stremio {
namespace requests {

inline std::string getCached(const std::string&, long, long) { return {}; }
inline std::string get(const std::string&, long) { return {}; }

}  // namespace requests
}  // namespace stremio
