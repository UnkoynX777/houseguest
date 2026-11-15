#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/core/analysis.hpp"
#include "houseguest/core/evaluator.hpp"
#include "houseguest/core/serialization.hpp"
#include "houseguest/core/subject.hpp"

using namespace houseguest::core;
using namespace std::chrono_literals;

namespace {

const TimePoint kNow = TimePoint{} + 1000h;
const Protection kProtection = Protection::windows_default("C:/Windows", "C:/Program Files");
const std::string kHashA(64, 'a');
const std::string kHashB(64, 'b');
const std::string kMojang = "CN=Mojang AB, O=Mojang AB, C=SE";

ProcessFacts facts(std::string path, std::string sha = {}, std::string publisher = {},
                   std::vector<std::string> lineage = {}) {
    ProcessFacts f;
    f.app = {std::move(path), std::move(sha), std::move(publisher), {}};
    f.hash_resolved = true;
    f.publisher_resolved = true;
    f.lineage = std::move(lineage);
    return f;
}

Component component(std::string id, ComponentRole role, std::string path, std::string sha = {},
                    std::string publisher = {}, Scope scope = Scope::Descendant) {
    Component c;
    c.id = std::move(id);
    c.role = role;
    c.scope = scope;
    c.path = std::move(path);
    c.sha256 = std::move(sha);
    c.publisher = std::move(publisher);
    return c;
}

Application minecraft(DependencyMode mode = DependencyMode::Approved) {
    Application app;
    app.id = "mc";
    app.name = "Minecraft";
    app.mode = mode;
    app.components = {component("main", ComponentRole::Main, "c:/games/mc/launcher.exe", kHashA, kMojang),
                      component("java", ComponentRole::Runtime, "c:/games/mc/runtime/bin/javaw.exe", kHashB)};
    return app;
}

Profile profile_with(Application app, Action fallback = Action::Deny) {
    Profile p{"p", "Games", fallback, {}, {}};
    p.apps.push_back(std::move(app));
    return p;
}

}  // namespace

TEST_CASE("the kernel decides first and no rule can override it", "[protection][safety]") {
    Profile p{
        "p", "x", Action::Deny, {Rule{"deny", Action::Deny, PathMatcher{"c:/**"}, 1000, std::nullopt}}, {}};
    auto f = facts("c:/anything/weird.exe");
    f.kernel_critical = true;
    CHECK(evaluate(p, kProtection, f, kNow).reason == "kernel_critical");
    f.kernel_critical = false;
    f.protected_process = true;
    CHECK(evaluate(p, kProtection, f, kNow).reason == "protected_process");
}

TEST_CASE("a critical process stays protected even when its image cannot be read", "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    ProcessFacts f;
    f.image_known = false;
    f.kernel_critical = true;
    CHECK(evaluate(p, kProtection, f, kNow).action == Action::Allow);
    f.kernel_critical = false;
    const auto d = evaluate(p, kProtection, f, kNow);
    CHECK(d.action == Action::Deny);
    CHECK(d.source == DecisionSource::Unresolved);
}

TEST_CASE("System32 is not protected as a whole", "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    for (const auto* name :
         {"curl.exe", "certutil.exe", "cmd.exe", "powershell.exe", "notepad.exe", "mshta.exe"}) {
        INFO(name);
        CHECK(evaluate(p, kProtection, facts(std::string("C:\\Windows\\System32\\") + name), kNow).action ==
              Action::Deny);
    }
    CHECK(evaluate(p, kProtection, facts("C:\\Windows\\System32\\svchost.exe"), kNow).source ==
          DecisionSource::Protected);
}

TEST_CASE("a protected name outside its directory is an ordinary program", "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    CHECK(evaluate(p, kProtection, facts("C:\\Users\\guest\\Downloads\\svchost.exe"), kNow).action ==
          Action::Deny);
    CHECK(evaluate(p, kProtection, facts("C:\\Windows\\Temp\\lsass.exe"), kNow).action == Action::Deny);
}

TEST_CASE("directory-wide protection needs a valid signature", "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    const std::string helper =
        "C:\\Windows\\System32\\DriverStore\\FileRepository\\nv.inf_amd64\\Display\\helper.exe";
    auto f = facts(helper);
    f.publisher_resolved = false;
    auto d = evaluate(p, kProtection, f, kNow);
    CHECK(d.source == DecisionSource::Protected);
    CHECK(d.needs_publisher);

    f.publisher_resolved = true;
    CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    f.app.publisher = "CN=NVIDIA Corporation";
    d = evaluate(p, kProtection, f, kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.reason == "driver_package");
}

TEST_CASE("Windows infrastructure needs the Windows signing identity", "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    const std::string windows =
        "CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";
    const std::string office = "CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, C=US";
    auto f = facts("C:\\Windows\\System32\\CompPkgSrv.exe");
    f.publisher_resolved = false;
    auto d = evaluate(p, kProtection, f, kNow);
    CHECK(d.source == DecisionSource::Protected);
    CHECK(d.needs_publisher);
    f.publisher_resolved = true;
    f.app.publisher = windows;
    d = evaluate(p, kProtection, f, kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.reason == "system_infrastructure");
    f.app.publisher = office;
    CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    f.app.publisher = "CN=Sordum Software";
    CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    f.app.publisher.clear();
    CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
}

TEST_CASE("a program that merely sits in System32 stays under the profile", "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    const std::string windows = "CN=Microsoft Windows, O=Microsoft Corporation";
    for (const auto* name : {"ReduceMemory_x64.exe", "wallpaperservice64.exe", "calc.exe", "mobsync.exe"}) {
        INFO(name);
        auto f = facts(std::string("C:\\Windows\\System32\\") + name, {}, windows);
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    }
}

TEST_CASE("a registered COM server started by the DCOM launcher is system infrastructure",
          "[protection][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    const std::string windows = "CN=Microsoft Windows, O=Microsoft Corporation";
    const std::string svchost = "C:\\Windows\\System32\\svchost.exe";
    auto f = facts("C:\\Windows\\System32\\mobsync.exe", {}, windows);
    f.com_server = true;
    f.parent_path = svchost;
    auto d = evaluate(p, kProtection, f, kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.reason == "windows_com_server");

    SECTION("a profile rule cannot turn it off") {
        Profile deny{
            "p", "x", Action::Deny, {Rule{"d", Action::Deny, PathMatcher{"c:/**"}, 1000, std::nullopt}}, {}};
        CHECK(evaluate(deny, kProtection, f, kNow).action == Action::Allow);
    }
    SECTION("without registration it is an ordinary program") {
        f.com_server = false;
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    }
    SECTION("started by the user's shell it is an ordinary program") {
        f.parent_path = "C:\\Windows\\explorer.exe";
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
        f.parent_path = "C:\\Users\\guest\\svchost.exe";
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
        f.parent_path.clear();
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    }
    SECTION("the signer must be Windows itself") {
        f.app.publisher = "CN=Microsoft Corporation, O=Microsoft Corporation";
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
        f.app.publisher = "CN=Skutta Software GmbH";
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
        f.publisher_resolved = false;
        CHECK(evaluate(p, kProtection, f, kNow).needs_publisher);
    }
    SECTION("tools that run arbitrary code never qualify") {
        for (const auto* name : {"cmd.exe", "powershell.exe", "rundll32.exe", "msiexec.exe", "mmc.exe"}) {
            INFO(name);
            auto tool = facts(std::string("C:\\Windows\\System32\\") + name, {}, windows);
            tool.com_server = true;
            tool.parent_path = svchost;
            CHECK(evaluate(p, kProtection, tool, kNow).action == Action::Deny);
        }
    }
    SECTION("only the operating system folders are considered") {
        auto elsewhere = facts("C:\\Windows\\Temp\\mobsync.exe", {}, windows);
        elsewhere.com_server = true;
        elsewhere.parent_path = svchost;
        CHECK(evaluate(p, kProtection, elsewhere, kNow).action == Action::Deny);
        elsewhere.app.path = "C:\\Users\\guest\\System32\\mobsync.exe";
        CHECK(evaluate(p, kProtection, elsewhere, kNow).action == Action::Deny);
    }
    SECTION("SysWOW64 servers qualify the same way") {
        f.app.path = "C:\\Windows\\SysWOW64\\mobsync.exe";
        CHECK(evaluate(p, kProtection, f, kNow).action == Action::Allow);
    }
}

TEST_CASE("Houseguest continuity is granted only through the verified facts", "[continuity][safety]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    auto f = facts("C:\\Program Files (x86)\\Microsoft\\EdgeWebView\\Application\\1\\msedgewebview2.exe");
    CHECK(evaluate(p, kProtection, f, kNow).action == Action::Deny);
    f.continuity = Continuity::WebView2;
    const auto d = evaluate(p, kProtection, f, kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.source == DecisionSource::Continuity);
}

TEST_CASE("the selected executable runs and reports its membership", "[application]") {
    const auto p = profile_with(minecraft());
    const auto d = evaluate(p, kProtection, facts("c:/games/mc/launcher.exe", kHashA, kMojang), kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.source == DecisionSource::Application);
    CHECK(d.app_id == "mc");
    REQUIRE(d.member_of.size() == 1);
    CHECK(d.member_of.front() == "mc");
}

TEST_CASE("an approved dependency runs only below its application", "[application][bypass]") {
    const auto p = profile_with(minecraft());
    const auto java = "c:/games/mc/runtime/bin/javaw.exe";
    CHECK(evaluate(p, kProtection, facts(java, kHashB), kNow).action == Action::Deny);

    const auto d = evaluate(p, kProtection, facts(java, kHashB, {}, {"mc"}), kNow);
    CHECK(d.action == Action::Allow);
    CHECK(d.source == DecisionSource::Dependency);
    CHECK(d.component_id == "java");
    CHECK(evaluate(p, kProtection, facts(java, kHashB, {}, {"other"}), kNow).action == Action::Deny);
}

TEST_CASE("a replaced dependency is refused and reported as changed", "[application][bypass]") {
    const auto p = profile_with(minecraft());
    const auto d =
        evaluate(p, kProtection, facts("c:/games/mc/runtime/bin/javaw.exe", kHashA, {}, {"mc"}), kNow);
    CHECK(d.action == Action::Deny);
    CHECK(d.note == "component_changed:java");
}

TEST_CASE("a renamed copy of a blocked tool does not inherit an approval", "[application][bypass]") {
    const auto p = profile_with(minecraft());
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/runtime/bin/javaw2.exe", kHashA, {}, {"mc"}), kNow)
              .action == Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/users/guest/cmd.exe", kHashB, {}, {"mc"}), kNow).action ==
          Action::Deny);
}

TEST_CASE("selected-only mode ignores the listed dependencies", "[application]") {
    const auto p = profile_with(minecraft(DependencyMode::SelectedOnly));
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/runtime/bin/javaw.exe", kHashB, {}, {"mc"}), kNow)
              .action == Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/launcher.exe", kHashA, kMojang), kNow).action ==
          Action::Allow);
}

TEST_CASE("signed updates are followed only when the component opts in", "[application]") {
    auto app = minecraft();
    app.components[0].follow_updates = true;
    const auto p = profile_with(app);
    const auto updated = evaluate(p, kProtection, facts("c:/games/mc/launcher.exe", kHashB, kMojang), kNow);
    CHECK(updated.action == Action::Allow);
    CHECK(updated.note == "component_updated:main");
    CHECK(
        evaluate(p, kProtection, facts("c:/games/mc/launcher.exe", kHashB, "CN=Someone Else"), kNow).action ==
        Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/launcher.exe", kHashB), kNow).action == Action::Deny);

    CHECK(evaluate(profile_with(minecraft()), kProtection, facts("c:/games/mc/launcher.exe", kHashB, kMojang),
                   kNow)
              .action == Action::Deny);
}

TEST_CASE("independent components need no ancestry but keep their pin", "[application]") {
    auto app = minecraft();
    app.components.push_back(component("broker", ComponentRole::Service, "c:/games/mc/anticheat.exe", kHashB,
                                       {}, Scope::Independent));
    const auto p = profile_with(app);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/anticheat.exe", kHashB), kNow).action == Action::Allow);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/anticheat.exe", kHashA), kNow).action == Action::Deny);
}

TEST_CASE("related mode admits same-signer descendants but never sensitive tools", "[application][bypass]") {
    const auto p = profile_with(minecraft(DependencyMode::Related));
    const auto helper =
        evaluate(p, kProtection, facts("c:/games/mc/updater/helper.exe", kHashB, kMojang, {"mc"}), kNow);
    CHECK(helper.action == Action::Allow);
    CHECK(helper.source == DecisionSource::Related);

    CHECK(evaluate(p, kProtection, facts("c:/games/mc/updater/helper.exe", kHashB, "CN=Other", {"mc"}), kNow)
              .action == Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/updater/helper.exe", kHashB, kMojang), kNow).action ==
          Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/cmd.exe", kHashB, kMojang, {"mc"}), kNow).action ==
          Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/games/mc/setup.exe", kHashB, kMojang, {"mc"}), kNow).action ==
          Action::Deny);
}

TEST_CASE("related mode on an unsigned application is limited to its folder", "[application]") {
    Application app;
    app.id = "indie";
    app.name = "Indie";
    app.mode = DependencyMode::Related;
    app.components = {component("main", ComponentRole::Main, "c:/games/indie/game.exe")};
    const auto p = profile_with(app);
    CHECK(evaluate(p, kProtection, facts("c:/games/indie/bin/tool.exe", {}, {}, {"indie"}), kNow).action ==
          Action::Allow);
    CHECK(evaluate(p, kProtection, facts("c:/users/guest/tool.exe", {}, {}, {"indie"}), kNow).action ==
          Action::Deny);
    CHECK(evaluate(p, kProtection, facts("c:/games/indie/bin/tool.exe"), kNow).action == Action::Deny);
}

TEST_CASE("an explicit deny above the application priority still wins", "[application][bypass]") {
    auto p = profile_with(minecraft());
    p.rules.push_back(
        Rule{"no-java", Action::Deny, PathMatcher{"c:/games/mc/runtime/**"}, 100, std::nullopt});
    const auto d =
        evaluate(p, kProtection, facts("c:/games/mc/runtime/bin/javaw.exe", kHashB, {}, {"mc"}), kNow);
    CHECK(d.action == Action::Deny);
    CHECK(d.rule_id == "no-java");
    CHECK(d.member_of.empty());
}

TEST_CASE("unknown descendants are queued for review without being allowed", "[application]") {
    const auto p = profile_with(minecraft());
    const auto d = evaluate(p, kProtection, facts("c:/tools/mystery.exe", kHashA, {}, {"mc"}), kNow);
    CHECK(d.action == Action::Deny);
    CHECK(d.review_app == "mc");

    auto quiet = minecraft();
    quiet.unknown = UnknownPolicy::Deny;
    CHECK(evaluate(profile_with(quiet), kProtection, facts("c:/tools/mystery.exe", kHashA, {}, {"mc"}), kNow)
              .review_app.empty());
}

TEST_CASE("expired components are ignored", "[application]") {
    auto app = minecraft();
    app.components[1].expires_at = kNow - 1h;
    CHECK(evaluate(profile_with(app), kProtection,
                   facts("c:/games/mc/runtime/bin/javaw.exe", kHashB, {}, {"mc"}), kNow)
              .action == Action::Deny);
}

TEST_CASE("unresolved hashes and signatures are requested, not guessed", "[application][engine]") {
    const auto p = profile_with(minecraft());
    auto f = facts("c:/games/mc/launcher.exe");
    f.hash_resolved = false;
    f.publisher_resolved = false;
    auto d = evaluate(p, kProtection, f, kNow);
    CHECK(d.needs_hash);

    f.hash_resolved = true;
    f.app.sha256 = kHashA;
    d = evaluate(p, kProtection, f, kNow);
    CHECK_FALSE(d.needs_hash);
    CHECK(d.action == Action::Allow);

    auto other = facts("c:/elsewhere/x.exe");
    other.hash_resolved = false;
    CHECK_FALSE(evaluate(p, kProtection, other, kNow).needs_hash);
}

TEST_CASE("ties are resolved the same way every time", "[engine]") {
    Profile p{"p", "x", Action::Deny, {}, {}};
    p.rules = {Rule{"b", Action::Allow, PathMatcher{"c:/t/**"}, 5, std::nullopt},
               Rule{"a", Action::Allow, PathMatcher{"c:/t/**"}, 5, std::nullopt}};
    for (int i = 0; i < 3; ++i) {
        std::ranges::reverse(p.rules);
        CHECK(evaluate(p, kProtection, facts("c:/t/x.exe"), kNow).rule_id == "a");
    }
}

TEST_CASE("analysis flags weak and broken application models", "[analysis][application]") {
    Application app;
    app.id = "x";
    app.name = "X";
    app.components = {component("m", ComponentRole::Main, "c:/x/x.exe"),
                      component("m", ComponentRole::Helper, "relative.exe"),
                      component("i", ComponentRole::Service, "c:/x/svc.exe", {}, {}, Scope::Independent),
                      component("sh", ComponentRole::Helper, "c:/x/cmd.exe", kHashA)};
    const auto findings = analyze(profile_with(app), kProtection, kNow);
    const auto has = [&](FindingCode code) {
        return std::ranges::any_of(findings, [&](const Finding& f) { return f.code == code; });
    };
    CHECK(has(FindingCode::DuplicateComponentId));
    CHECK(has(FindingCode::InvalidComponent));
    CHECK(has(FindingCode::WeakIndependent));
    CHECK(has(FindingCode::UnpinnedComponent));
    CHECK(has(FindingCode::SensitiveComponent));
    CHECK(has_errors(findings));

    Application empty;
    empty.id = "e";
    empty.name = "E";
    CHECK(std::ranges::any_of(analyze(profile_with(empty), kProtection, kNow), [](const Finding& f) {
        return f.code == FindingCode::ApplicationWithoutMain;
    }));
}

TEST_CASE("analysis reports an application blocked by a stronger deny rule", "[analysis][application]") {
    auto p = profile_with(minecraft());
    p.rules.push_back(Rule{"deny-mc", Action::Deny, PathMatcher{"c:/games/**"}, 80, std::nullopt});
    CHECK(std::ranges::any_of(analyze(p, kProtection, kNow), [](const Finding& f) {
        return f.code == FindingCode::ApplicationBlockedByRule;
    }));
}

TEST_CASE("the protected table is reviewed, not accidental", "[protection][safety]") {
    const auto patterns = kProtection.patterns();
    CHECK(patterns.size() == 60);
    for (const auto& pattern : patterns) {
        INFO(pattern);
        CHECK(pattern != "c:/windows/system32/**");
        CHECK(pattern.find("cmd.exe") == std::string::npos);
        CHECK(pattern.find("powershell") == std::string::npos);
    }
}

TEST_CASE("signer subjects compare in any attribute order", "[subject]") {
    CHECK(same_subject("CN=Contoso, O=Contoso Ltd, C=US", "C=US, O=Contoso Ltd, CN=Contoso"));
    CHECK(same_subject("cn=contoso, o=contoso ltd", "O=Contoso Ltd,CN=Contoso"));
    CHECK_FALSE(same_subject("CN=Contoso, O=Contoso Ltd", "CN=Contoso"));
    CHECK_FALSE(same_subject("CN=Contoso", "CN=Contoso Evil"));
    CHECK_FALSE(same_subject("", ""));
    CHECK(same_subject(R"(CN="Contoso, Inc.", C=US)", R"(C=US, CN="Contoso, Inc.")"));
    CHECK(subject_has("C=US, O=Microsoft Corporation, CN=Microsoft Corporation", "CN",
                      "microsoft corporation"));
    CHECK_FALSE(subject_has("O=Microsoft Corporation, CN=Evil", "CN", "Microsoft Corporation"));
}

TEST_CASE("the built-in Windows programs entry needs both the signer and the Windows folder",
          "[subject][engine]") {
    auto profile = builtin_profiles().front();
    const auto expand = [](std::string& pattern) {
        const auto at = pattern.find("%SystemRoot%");
        if (at != std::string::npos) pattern.replace(at, 12, "c:/windows");
    };
    for (auto& rule : profile.rules) {
        if (auto* path = std::get_if<PathMatcher>(&rule.matcher)) expand(path->pattern);
    }
    for (auto& app : profile.apps) {
        for (auto& component : app.components) expand(component.path);
    }
    const std::string reported =
        "C=US, S=Washington, L=Redmond, O=Microsoft Corporation, CN=Microsoft Windows";
    CHECK(evaluate(profile, kProtection, facts("c:/windows/system32/calc.exe", {}, reported), kNow).action ==
          Action::Allow);
    CHECK(evaluate(profile, kProtection, facts("c:/windows/system32/cmd.exe", {}, reported), kNow).action ==
          Action::Deny);
    CHECK(evaluate(profile, kProtection, facts("c:/windows/system32/certutil.exe", {}, reported), kNow)
              .action == Action::Deny);
    CHECK(evaluate(profile, kProtection, facts("c:/users/guest/calc.exe", {}, "CN=Someone Else"), kNow)
              .action == Action::Deny);
    CHECK(evaluate(profile, kProtection, facts("c:/users/guest/calc-copy.exe", {}, reported), kNow).action ==
          Action::Deny);
    CHECK(evaluate(profile, kProtection, facts("c:/users/guest/cmd2.exe", {}, reported), kNow).action ==
          Action::Deny);
}

TEST_CASE("a stored profile with the old Windows signer rule is upgraded on load",
          "[serialization][bypass]") {
    const auto legacy = Json::parse(R"({
        "id":"p","name":"Old","default_action":"deny",
        "rules":[{"id":"windows-signed","action":"allow","priority":10,
                  "match":{"type":"publisher","value":"CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US"}}]
    })");
    const auto upgraded = profile_from_json(legacy);
    CHECK(upgraded.rules.empty());
    REQUIRE(upgraded.apps.size() == 1);
    CHECK(upgraded.apps.front().id == "builtin-windows");
    CHECK(profile_from_json(to_json(upgraded)).apps.size() == 1);
}
