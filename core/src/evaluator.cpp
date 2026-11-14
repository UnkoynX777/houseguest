#include "houseguest/core/evaluator.hpp"

#include <algorithm>
#include <cctype>

#include "houseguest/core/glob.hpp"

namespace houseguest::core {
namespace {

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(
        a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

}  // namespace

Baseline Baseline::windows_default(const std::string& system_root, const std::string& install_dir) {
    Baseline b;
    const auto root = normalize_path(system_root);
    constexpr const char* kSystem32[] = {
        "csrss.exe",   "smss.exe",     "wininit.exe",     "winlogon.exe", "services.exe",     "lsass.exe",
        "svchost.exe", "dwm.exe",      "fontdrvhost.exe", "sihost.exe",   "taskhostw.exe",    "ctfmon.exe",
        "conhost.exe", "userinit.exe", "logonui.exe",     "dllhost.exe",  "runtimebroker.exe"};
    for (const char* name : kSystem32) b.path_patterns.push_back(root + "/system32/" + name);
    b.path_patterns.push_back(root + "/explorer.exe");
    b.path_patterns.push_back(normalize_path(install_dir) + "/**");
    return b;
}

bool matcher_matches(const Matcher& matcher, const AppIdentity& app) {
    return std::visit(
        Overloaded{[&](const PathMatcher& m) { return path_matches(m.pattern, app.path); },
                   [&](const HashMatcher& m) { return !app.sha256.empty() && iequals(m.sha256, app.sha256); },
                   [&](const PublisherMatcher& m) {
                       return !app.publisher.empty() && iequals(m.subject, app.publisher);
                   },
                   [&](const PackageMatcher& m) {
                       return !app.package_family.empty() && iequals(m.family_name, app.package_family);
                   }},
        matcher);
}

bool rule_is_active(const Rule& rule, TimePoint now) {
    return !rule.expires_at || now < *rule.expires_at;
}

Decision evaluate(const Profile& profile, const Baseline& baseline, const AppIdentity& app, TimePoint now) {
    for (const auto& pattern : baseline.path_patterns) {
        if (path_matches(pattern, app.path)) {
            return {Action::Allow, DecisionSource::Baseline, {}};
        }
    }

    const Rule* best = nullptr;
    for (const auto& rule : profile.rules) {
        if (!rule_is_active(rule, now) || !matcher_matches(rule.matcher, app)) continue;
        if (!best || rule.priority > best->priority ||
            (rule.priority == best->priority && rule.action == Action::Deny &&
             best->action == Action::Allow)) {
            best = &rule;
        }
    }
    if (best) return {best->action, DecisionSource::Rule, best->id, best->priority};
    return {profile.default_action, DecisionSource::Default, {}};
}

SimulationReport simulate(const Profile& profile, const Baseline& baseline,
                          const std::vector<AppIdentity>& apps, TimePoint now) {
    SimulationReport report;
    report.apps.reserve(apps.size());
    for (const auto& app : apps) {
        auto decision = evaluate(profile, baseline, app, now);
        (decision.action == Action::Allow ? report.allowed : report.denied)++;
        report.apps.push_back({app, std::move(decision)});
    }
    return report;
}

}  // namespace houseguest::core
