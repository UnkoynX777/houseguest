#include <catch2/catch_test_macros.hpp>

#include "houseguest/core/analysis.hpp"
#include "houseguest/core/serialization.hpp"

using namespace houseguest::core;

TEST_CASE("profiles survive a JSON round trip", "[serialization]") {
    for (const auto& profile : builtin_profiles()) {
        const auto copy = profile_from_json(to_json(profile));
        CHECK(to_json(copy) == to_json(profile));
    }
}

TEST_CASE("expiry is preserved", "[serialization]") {
    Rule rule{"r", Action::Allow, HashMatcher{std::string(64, 'a')}, 3,
              TimePoint{std::chrono::seconds{1234}}};
    const auto copy = rule_from_json(to_json(rule));
    REQUIRE(copy.expires_at.has_value());
    CHECK(*copy.expires_at == *rule.expires_at);
}

TEST_CASE("malformed input is rejected", "[serialization]") {
    CHECK_THROWS_AS(profile_from_json(Json::parse(R"({"id":"a"})")), std::invalid_argument);
    CHECK_THROWS_AS(profile_from_json(Json::parse(R"([])")), std::invalid_argument);
    CHECK_THROWS_AS(
        rule_from_json(Json::parse(R"({"id":"a","action":"maybe","match":{"type":"path","value":"c:/x"}})")),
        std::invalid_argument);
    CHECK_THROWS_AS(
        rule_from_json(Json::parse(
            R"({"id":"a","action":"allow","priority":99999,"match":{"type":"path","value":"c:/x"}})")),
        std::invalid_argument);
    CHECK_THROWS_AS(
        rule_from_json(Json::parse(R"({"id":"a","action":"allow","match":{"type":"magic","value":"x"}})")),
        std::invalid_argument);
}

TEST_CASE("built-in templates never trip the baseline safety checks", "[serialization][safety]") {
    const auto baseline = Baseline::windows_default("c:\\windows", "c:\\program files\\houseguest");
    for (auto profile : builtin_profiles()) {
        for (auto& rule : profile.rules) {
            if (auto* path = std::get_if<PathMatcher>(&rule.matcher)) {
                for (const auto& [token, value] :
                     {std::pair<std::string, std::string>{"%SystemRoot%", "c:\\windows"},
                      {"%SystemDrive%", "c:"}}) {
                    if (const auto at = path->pattern.find(token); at != std::string::npos) {
                        path->pattern.replace(at, token.size(), value);
                    }
                }
            }
        }
        for (const auto& finding : analyze(profile, baseline, TimePoint{} + std::chrono::hours(1))) {
            INFO(profile.name << ": " << finding.message);
            CHECK(finding.severity == Severity::Info);
        }
    }
}
