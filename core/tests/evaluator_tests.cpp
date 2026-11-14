#include <catch2/catch_test_macros.hpp>

#include "houseguest/core/evaluator.hpp"

using namespace houseguest::core;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = TimePoint{} + 1000h;
const Protection kBaseline = Protection::windows_default("C:/Windows", "C:/Program Files");

Rule rule(std::string id, Action a, Matcher m, int priority = 0) {
    return Rule{std::move(id), a, std::move(m), priority, std::nullopt};
}

AppIdentity app(std::string path) {
    return AppIdentity{std::move(path), {}, {}, {}};
}

}  // namespace

TEST_CASE("default action applies when nothing matches", "[evaluator]") {
    Profile p{"p", "Guest", Action::Deny, {}};
    const auto d = evaluate(p, kBaseline, app("c:/tools/a.exe"), kNow);
    CHECK(d.action == Action::Deny);
    CHECK(d.source == DecisionSource::Default);
}

TEST_CASE("allow rule opens a single application", "[evaluator]") {
    Profile p{"p", "Guest", Action::Deny, {rule("mc", Action::Allow, PathMatcher{"c:/games/minecraft/**"})}};
    CHECK(evaluate(p, kBaseline, app("C:\\Games\\Minecraft\\javaw.exe"), kNow).action == Action::Allow);
    CHECK(evaluate(p, kBaseline, app("c:/games/other/x.exe"), kNow).action == Action::Deny);
}

TEST_CASE("higher priority wins and deny wins ties", "[evaluator]") {
    Profile p{"p",
              "x",
              Action::Allow,
              {rule("a", Action::Allow, PathMatcher{"c:/t/**"}, 5),
               rule("d", Action::Deny, PathMatcher{"c:/t/bad.exe"}, 5),
               rule("a2", Action::Allow, PathMatcher{"c:/t/bad.exe"}, 9)}};
    CHECK(evaluate(p, kBaseline, app("c:/t/bad.exe"), kNow).rule_id == "a2");
    p.rules.pop_back();
    const auto d = evaluate(p, kBaseline, app("c:/t/bad.exe"), kNow);
    CHECK(d.action == Action::Deny);
    CHECK(d.rule_id == "d");
}

TEST_CASE("expired rules are ignored", "[evaluator]") {
    auto r = rule("tmp", Action::Allow, PathMatcher{"c:/t/**"});
    r.expires_at = kNow - 1h;
    Profile p{"p", "x", Action::Deny, {r}};
    CHECK(evaluate(p, kBaseline, app("c:/t/a.exe"), kNow).action == Action::Deny);
    CHECK(evaluate(p, kBaseline, app("c:/t/a.exe"), kNow - 2h).action == Action::Allow);
}

TEST_CASE("protected Windows components cannot be denied", "[evaluator][safety]") {
    Profile p{"p", "x", Action::Deny, {rule("deny-all", Action::Deny, PathMatcher{"c:/**"}, 100)}};
    const auto d = evaluate(p, kBaseline, app("C:/Windows/System32/winlogon.exe"), kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.source == DecisionSource::Protected);
    CHECK(d.reason == "windows_core");
    CHECK(evaluate(p, kBaseline, app("C:/Windows/explorer.exe"), kNow).source == DecisionSource::Protected);
}

TEST_CASE("Houseguest install folder is not protected by path alone", "[evaluator][safety]") {
    Profile p{"p", "x", Action::Deny, {}};
    CHECK(evaluate(p, kBaseline, app("C:/Program Files/Houseguest/houseguest.exe"), kNow).action ==
          Action::Deny);
}

TEST_CASE("admin tools are not part of the baseline", "[evaluator][safety]") {
    Profile p{"p", "x", Action::Deny, {}};
    CHECK(evaluate(p, kBaseline, app("C:\\Windows\\System32\\cmd.exe"), kNow).action == Action::Deny);
    CHECK(evaluate(p, kBaseline, app("C:\\Windows\\regedit.exe"), kNow).action == Action::Deny);
}

TEST_CASE("hash, publisher and package matchers survive renaming", "[evaluator]") {
    Profile p{"p",
              "x",
              Action::Deny,
              {rule("h", Action::Allow, HashMatcher{std::string(64, 'a')}),
               rule("pub", Action::Allow, PublisherMatcher{"Discord Inc."}),
               rule("pkg", Action::Allow, PackageMatcher{"Mojang.Minecraft_abc"})}};
    AppIdentity renamed{"c:/anywhere/renamed.exe", std::string(64, 'A'), {}, {}};
    CHECK(evaluate(p, kBaseline, renamed, kNow).rule_id == "h");
    CHECK(evaluate(p, kBaseline, AppIdentity{"c:/x.exe", {}, "discord inc.", {}}, kNow).rule_id == "pub");
    CHECK(evaluate(p, kBaseline, AppIdentity{"c:/x.exe", {}, {}, "Mojang.Minecraft_abc"}, kNow).rule_id ==
          "pkg");
    CHECK(evaluate(p, kBaseline, AppIdentity{"c:/x.exe", {}, {}, {}}, kNow).action == Action::Deny);
}

TEST_CASE("simulation counts decisions without side effects", "[evaluator]") {
    Profile p{"p", "x", Action::Deny, {rule("a", Action::Allow, PathMatcher{"c:/ok/**"})}};
    const auto r = simulate(p, kBaseline, {app("c:/ok/1.exe"), app("c:/no/2.exe"), app("c:/ok/3.exe")}, kNow);
    CHECK(r.allowed == 2);
    CHECK(r.denied == 1);
    CHECK(r.apps.size() == 3);
}

TEST_CASE("decisions report the priority of the winning rule", "[evaluator]") {
    Profile p{"p", "x", Action::Deny, {rule("a", Action::Allow, PathMatcher{"c:/t/**"}, 42)}};
    CHECK(evaluate(p, kBaseline, app("c:/t/a.exe"), kNow).priority == 42);
}
