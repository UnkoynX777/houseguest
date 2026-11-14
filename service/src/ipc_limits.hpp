#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

namespace houseguest::service {

inline constexpr int kMaxJsonDepth = 32;

[[nodiscard]] bool nesting_within(std::string_view json, int limit);

class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    TokenBucket(double per_second, double burst, Clock::time_point now = Clock::now());

    [[nodiscard]] bool take(Clock::time_point now = Clock::now());

private:
    double rate_;
    double capacity_;
    double tokens_;
    Clock::time_point updated_;
};

}  // namespace houseguest::service
