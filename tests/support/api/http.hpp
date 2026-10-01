#pragma once

#include <string>

// Parser tests do not perform HTTP requests. This narrow shim avoids pulling
// curl and Borealis headers into the standalone test binary.
class HTTP {
public:
    inline static constexpr long TIMEOUT = 3000L;

    static std::string get(const std::string&, long) { return {}; }
};
