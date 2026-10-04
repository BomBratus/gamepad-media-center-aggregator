#pragma once

#include <string>
#include <functional>
#include <memory>
#include <atomic>

// Parser tests do not perform HTTP requests. This narrow shim avoids pulling
// curl and Borealis headers into the standalone test binary.
class HTTP {
public:
    inline static constexpr long TIMEOUT = 3000L;

    using Cancel = std::shared_ptr<std::atomic_bool>;
    struct Timeout { long timeout; };
    inline static std::function<std::string(const std::string&, const Cancel&)> transport;
    static std::string get(const std::string&, long) { return {}; }
    static std::string get(const std::string& url, Timeout, const Cancel& cancel = {}) {
        return transport ? transport(url, cancel) : std::string{};
    }
};
