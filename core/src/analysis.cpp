#include "houseguest/core/analysis.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "houseguest/core/glob.hpp"

namespace houseguest::core {
namespace {

bool is_hex_sha256(const std::string& s) {
    return s.size() == 64 && std::ranges::all_of(s, [](unsigned char c) { return std::isxdigit(c); });
}

bool matcher_is_valid(const Matcher& m) {
    return std::visit(
        [](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, PathMatcher>)
                return is_valid_path_pattern(v.pattern);
            else if constexpr (std::is_same_v<T, HashMatcher>)
                return is_hex_sha256(v.sha256);
            else if constexpr (std::is_same_v<T, PublisherMatcher>)
                return !v.subject.empty();
            else
                return !v.family_name.empty();
        },
        m);
}

std::string matcher_key(const Matcher& m) {
    return std::visit(
        [&](const auto& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            std::string key = std::to_string(m.index()) + ":";
            if constexpr (std::is_same_v<T, PathMatcher>)
                return key + normalize_path(v.pattern);
            else if constexpr (std::is_same_v<T, HashMatcher>)
                return key + v.sha256;
            else if constexpr (std::is_same_v<T, PublisherMatcher>)
                return key + v.subject;
            else
                return key + v.family_name;
        },
        m);
}

bool denies_protected(const PathMatcher& m, const Protection& protection) {
    return std::ranges::any_of(protection.patterns(), [&](const std::string& entry) {
        const auto wildcard = entry.find_first_of("*?");
        auto concrete = wildcard == std::string::npos ? entry : entry.substr(0, entry.rfind('/', wildcard));
        return path_matches(m.pattern, concrete);
    });
}

void analyze_applications(const Profile& profile, const Protection&, TimePoint now,
                          std::vector<Finding>& out) {
    std::set<std::string> app_ids;
    for (const auto& app : profile.apps) {
        if (!app_ids.insert(app.id).second)
            out.push_back({Severity::Error,
                           FindingCode::DuplicateRuleId,
                           {app.id},
                           "Application id is used more than once."});
        std::set<std::string> component_ids;
        bool has_main = false;
        bool main_signed = false;
        for (const auto& c : app.components) {
            has_main |= c.role == ComponentRole::Main;
            main_signed |= c.role == ComponentRole::Main && !c.publisher.empty();
            if (!component_ids.insert(c.id).second)
                out.push_back({Severity::Error,
                               FindingCode::DuplicateComponentId,
                               {app.id, c.id},
                               "Component id is used more than once in this application."});
            const bool path_ok = c.path.empty() || is_valid_path_pattern(c.path);
            const bool hash_ok = c.sha256.empty() || is_hex_sha256(c.sha256);
            if (!path_ok || !hash_ok || (c.path.empty() && c.package_family.empty()))
                out.push_back({Severity::Error,
                               FindingCode::InvalidComponent,
                               {app.id, c.id},
                               "Component identity is malformed and would never match."});
            if (!component_is_active(c, now)) {
                out.push_back({Severity::Info,
                               FindingCode::ExpiredComponent,
                               {app.id, c.id},
                               "Component approval has expired and is ignored."});
                continue;
            }
            const bool pinned = !c.sha256.empty() || !c.publisher.empty();
            if (c.role != ComponentRole::Main && c.scope == Scope::Independent && !pinned)
                out.push_back(
                    {Severity::Error,
                     FindingCode::WeakIndependent,
                     {app.id, c.id},
                     "A component that can start outside the application needs a hash or signer pin."});
            else if (!pinned && c.package_family.empty())
                out.push_back(
                    {Severity::Info,
                     FindingCode::UnpinnedComponent,
                     {app.id, c.id},
                     "Identified by path only. Pin a hash or signer so a replaced file is noticed."});
            if (c.role != ComponentRole::Main && is_sensitive_tool(c.path))
                out.push_back(
                    {Severity::Warning,
                     FindingCode::SensitiveComponent,
                     {app.id, c.id},
                     "This component is a tool that can run other programs. Approve it only when the "
                     "application cannot work without it."});
        }
        if (!has_main)
            out.push_back({Severity::Error,
                           FindingCode::ApplicationWithoutMain,
                           {app.id},
                           "The application has no main executable."});
        if (app.mode == DependencyMode::Related && !main_signed)
            out.push_back({Severity::Info,
                           FindingCode::RelatedWithoutSigner,
                           {app.id},
                           "Related mode on an unsigned application relies on the install folder. Pin its "
                           "components instead when possible."});
        for (const auto& c : app.components) {
            if (c.role != ComponentRole::Main || c.path.empty() || !component_is_active(c, now)) continue;
            for (const auto& rule : profile.rules) {
                const auto* path = std::get_if<PathMatcher>(&rule.matcher);
                if (rule.action != Action::Deny || !rule_is_active(rule, now) ||
                    rule.priority < app.priority || !path)
                    continue;
                if (path_matches(path->pattern, c.path) || path_matches(c.path, path->pattern))
                    out.push_back(
                        {Severity::Warning,
                         FindingCode::ApplicationBlockedByRule,
                         {app.id, rule.id},
                         "A Deny rule with equal or higher priority also covers this application, so "
                         "it stays blocked."});
            }
        }
    }
}

}  // namespace

std::vector<Finding> analyze(const Profile& profile, const Protection& protection, TimePoint now) {
    std::vector<Finding> out;
    std::set<std::string> seen_ids;

    for (const auto& r : profile.rules) {
        if (!seen_ids.insert(r.id).second) {
            out.push_back(
                {Severity::Error, FindingCode::DuplicateRuleId, {r.id}, "Rule id is used more than once."});
        }
        if (!matcher_is_valid(r.matcher)) {
            out.push_back({Severity::Error,
                           FindingCode::InvalidMatcher,
                           {r.id},
                           "Rule matcher is malformed and would never match."});
        }
        if (!rule_is_active(r, now)) {
            out.push_back(
                {Severity::Info, FindingCode::ExpiredRule, {r.id}, "Rule has expired and is ignored."});
            continue;
        }
        if (r.action == Action::Deny) {
            if (const auto* p = std::get_if<PathMatcher>(&r.matcher);
                p && is_valid_path_pattern(p->pattern) && denies_protected(*p, protection)) {
                out.push_back({Severity::Warning,
                               FindingCode::BaselineProtected,
                               {r.id},
                               "Rule overlaps Windows components that are always protected so the "
                               "system stays usable and recoverable; those components stay allowed."});
            }
        }
    }

    for (std::size_t i = 0; i < profile.rules.size(); ++i) {
        const auto& a = profile.rules[i];
        if (!rule_is_active(a, now)) continue;
        for (std::size_t j = i + 1; j < profile.rules.size(); ++j) {
            const auto& b = profile.rules[j];
            if (!rule_is_active(b, now) || matcher_key(a.matcher) != matcher_key(b.matcher)) continue;
            if (a.action != b.action && a.priority == b.priority) {
                out.push_back({Severity::Warning,
                               FindingCode::ConflictingRules,
                               {a.id, b.id},
                               "Allow and Deny rules match the same target at equal priority; "
                               "Deny wins."});
            } else if (a.priority != b.priority) {
                const auto& low = a.priority < b.priority ? a : b;
                const auto& high = a.priority < b.priority ? b : a;
                out.push_back({Severity::Info,
                               FindingCode::ShadowedRule,
                               {low.id, high.id},
                               "Rule is overridden by a higher-priority rule on the same target."});
            }
        }
    }

    analyze_applications(profile, protection, now, out);

    if (profile.default_action == Action::Deny) {
        const bool any_allow =
            !profile.apps.empty() || std::ranges::any_of(profile.rules, [&](const Rule& r) {
                return r.action == Action::Allow && rule_is_active(r, now);
            });
        if (!any_allow) {
            out.push_back({Severity::Warning,
                           FindingCode::NoAllowances,
                           {},
                           "Default is Deny and no Allow rule is active: only essential system "
                           "components can run."});
        }
    }
    return out;
}

bool has_errors(const std::vector<Finding>& findings) {
    return std::ranges::any_of(findings, [](const Finding& f) { return f.severity == Severity::Error; });
}

}  // namespace houseguest::core
