#include "ipc_limits.hpp"

#include <algorithm>

namespace houseguest::service {

bool nesting_within(std::string_view json, int limit) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : json) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '[' || c == '{') {
            if (++depth > limit) return false;
        } else if (c == ']' || c == '}') {
            depth = std::max(0, depth - 1);
        }
    }
    return true;
}

TokenBucket::TokenBucket(double per_second, double burst, Clock::time_point now)
    : rate_(per_second), capacity_(burst), tokens_(burst), updated_(now) {}

bool TokenBucket::take(Clock::time_point now) {
    const std::chrono::duration<double> elapsed = now - updated_;
    updated_ = now;
    tokens_ = std::min(capacity_, tokens_ + elapsed.count() * rate_);
    if (tokens_ < 1.0) return false;
    tokens_ -= 1.0;
    return true;
}

}  // namespace houseguest::service
