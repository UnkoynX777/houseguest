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

bool denies_baseline(const PathMatcher& m, const Baseline& baseline) {
    return std::ranges::any_of(baseline.path_patterns, [&](const std::string& entry) {
        auto concrete = entry;
        if (concrete.ends_with("/**")) concrete.resize(concrete.size() - 3);
        return path_matches(m.pattern, concrete);
    });
}

}  // namespace

std::vector<Finding> analyze(const Profile& profile, const Baseline& baseline, TimePoint now) {
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
                p && is_valid_path_pattern(p->pattern) && denies_baseline(*p, baseline)) {
                out.push_back({Severity::Warning,
                               FindingCode::BaselineProtected,
                               {r.id},
                               "Rule overlaps components that are always allowed to protect "
                               "system recovery; those components stay allowed."});
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

    if (profile.default_action == Action::Deny) {
        const bool any_allow = std::ranges::any_of(profile.rules, [&](const Rule& r) {
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
