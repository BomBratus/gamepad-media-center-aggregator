#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace gmca {
struct RequestState {
    inline static std::atomic<uint64_t> sequence{0};
    const uint64_t id = ++sequence;
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
};
using RequestToken = std::shared_ptr<RequestState>;
inline thread_local RequestToken currentRequest;
inline bool cancelled(const RequestToken& token) { return token && token->cancel->load(); }
inline void checkRequest() {
    if (cancelled(currentRequest)) throw std::runtime_error("Request cancelled");
}
class RequestBinding {
public:
    explicit RequestBinding(RequestToken token) : previous(std::move(currentRequest)) { currentRequest = std::move(token); }
    ~RequestBinding() { currentRequest = std::move(previous); }
private:
    RequestToken previous;
};
// UI-owned lifetime/generation. Replacing a request cancels its HTTP and queued
// work; callbacks still release Borealis lifetime bookkeeping before checking.
class LatestRequest {
public:
    ~LatestRequest() { cancel(); }
    RequestToken next() { cancel(); token = std::make_shared<RequestState>(); return token; }
    void cancel() { if (token) token->cancel->store(true); }
private:
    RequestToken token;
};
} // namespace gmca
