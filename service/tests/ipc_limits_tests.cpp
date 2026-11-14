#include <chrono>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ipc_limits.hpp"

using namespace houseguest::service;
using namespace std::chrono_literals;

TEST_CASE("nesting limit rejects deep documents", "[ipc][security]") {
    CHECK(nesting_within(R"({"cmd":"status","args":{"a":[1,2,{"b":null}]}})", kMaxJsonDepth));
    CHECK_FALSE(nesting_within(std::string(100000, '['), kMaxJsonDepth));
    CHECK_FALSE(nesting_within(std::string(kMaxJsonDepth + 1, '{'), kMaxJsonDepth));
    CHECK(nesting_within(std::string(kMaxJsonDepth, '['), kMaxJsonDepth));
}

TEST_CASE("brackets inside strings do not count as nesting", "[ipc][security]") {
    std::string text = "{\"a\":\"";
    text.append(500, '[');
    text += "\\\"[[[[\"}";
    CHECK(nesting_within(text, kMaxJsonDepth));
}

TEST_CASE("token bucket limits bursts and refills over time", "[ipc][security]") {
    const auto start = TokenBucket::Clock::now();
    TokenBucket bucket(10, 3, start);
    CHECK(bucket.take(start));
    CHECK(bucket.take(start));
    CHECK(bucket.take(start));
    CHECK_FALSE(bucket.take(start));
    CHECK_FALSE(bucket.take(start + 50ms));
    CHECK(bucket.take(start + 150ms));
    CHECK_FALSE(bucket.take(start + 150ms));
}
