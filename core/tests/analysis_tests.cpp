#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/core/analysis.hpp"

using namespace houseguest::core;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = TimePoint{} + 1000h;
const Baseline kBaseline = Baseline::windows_default("C:\\Windows", "C:\\Program Files\\Houseguest");

Rule rule(std::string id, Action a, Matcher m, int priority = 0) {
    return Rule{std::move(id), a, std::move(m), priority, std::nullopt};
}

bool has(const std::vector<Finding>& f, FindingCode c) {
    return std::ranges::any_of(f, [&](const Finding& x) { return x.code == c; });
}

}  // namespace

TEST_CASE("malformed matchers and duplicate ids are errors", "[analysis]") {
    Profile p{"p",
              "x",
              Action::Deny,
              {rule("a", Action::Allow, PathMatcher{"relative.exe"}),
               rule("a", Action::Allow, HashMatcher{"xyz"})}};
    const auto f = analyze(p, kBaseline, kNow);
    CHECK(has(f, FindingCode::InvalidMatcher));
    CHECK(has(f, FindingCode::DuplicateRuleId));
    CHECK(has_errors(f));
}

TEST_CASE("equal-priority allow/deny on the same target is a conflict", "[analysis]") {
    Profile p{"p",
              "x",
              Action::Deny,
              {rule("a", Action::Allow, PathMatcher{"c:/t/x.exe"}),
               rule("d", Action::Deny, PathMatcher{"C:\\T\\x.exe"})}};
    const auto f = analyze(p, kBaseline, kNow);
    CHECK(has(f, FindingCode::ConflictingRules));
    CHECK_FALSE(has_errors(f));
}

TEST_CASE("different priority on the same target is reported as shadowing", "[analysis]") {
    Profile p{"p",
              "x",
              Action::Deny,
              {rule("lo", Action::Allow, PathMatcher{"c:/t/x.exe"}, 1),
               rule("hi", Action::Deny, PathMatcher{"c:/t/x.exe"}, 2)}};
    const auto f = analyze(p, kBaseline, kNow);
    REQUIRE(has(f, FindingCode::ShadowedRule));
    CHECK_FALSE(has(f, FindingCode::ConflictingRules));
}

TEST_CASE("a deny rule that overlaps the baseline is flagged", "[analysis][safety]") {
    Profile p{"p", "x", Action::Allow, {rule("all", Action::Deny, PathMatcher{"c:/**"})}};
    CHECK(has(analyze(p, kBaseline, kNow), FindingCode::BaselineProtected));
}

TEST_CASE("default-deny profile without allowances warns", "[analysis][safety]") {
    Profile p{"p", "x", Action::Deny, {}};
    CHECK(has(analyze(p, kBaseline, kNow), FindingCode::NoAllowances));
    p.rules.push_back(rule("a", Action::Allow, PathMatcher{"c:/ok/**"}));
    CHECK_FALSE(has(analyze(p, kBaseline, kNow), FindingCode::NoAllowances));
}

TEST_CASE("expired rules are informational and do not count as allowances", "[analysis]") {
    auto r = rule("tmp", Action::Allow, PathMatcher{"c:/ok/**"});
    r.expires_at = kNow - 1h;
    Profile p{"p", "x", Action::Deny, {r}};
    const auto f = analyze(p, kBaseline, kNow);
    CHECK(has(f, FindingCode::ExpiredRule));
    CHECK(has(f, FindingCode::NoAllowances));
}
