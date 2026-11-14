#include "houseguest/core/evaluator.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <tuple>

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

int source_rank(DecisionSource source) {
    switch (source) {
        case DecisionSource::Rule:
            return 0;
        case DecisionSource::Application:
            return 1;
        case DecisionSource::Dependency:
            return 2;
        case DecisionSource::Related:
            return 3;
        default:
            return 4;
    }
}

bool outranks(const Decision& a, const Decision& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    if (a.action != b.action) return a.action == Action::Deny;
    if (source_rank(a.source) != source_rank(b.source)) return source_rank(a.source) < source_rank(b.source);
    return std::tie(a.rule_id, a.app_id, a.component_id) < std::tie(b.rule_id, b.app_id, b.component_id);
}

struct Deferred {
    int priority;
    Action action;
    bool hash;
    bool publisher;
};

class Ranking {
public:
    void offer(Decision candidate) {
        if (!best_ || outranks(candidate, *best_)) best_ = std::move(candidate);
    }
    void defer(const Deferred& item) { deferred_.push_back(item); }
    void note(std::string text) {
        if (note_.empty()) note_ = std::move(text);
    }
    void member(const std::string& app_id) {
        if (std::ranges::find(members_, app_id) == members_.end()) members_.push_back(app_id);
    }

    Decision settle(Action fallback) {
        Decision decision;
        if (best_) {
            decision = std::move(*best_);
        } else {
            decision.action = fallback;
            decision.source = DecisionSource::Default;
            decision.reason = fallback == Action::Allow ? "default.allow" : "default.deny";
            decision.priority = std::numeric_limits<int>::min();
        }
        for (const auto& item : deferred_) {
            const bool changes = best_ ? item.priority > decision.priority ||
                                             (item.priority == decision.priority &&
                                              item.action == Action::Deny && decision.action == Action::Allow)
                                       : item.action != fallback;
            if (!changes) continue;
            decision.needs_hash |= item.hash;
            decision.needs_publisher |= item.publisher;
        }
        decision.note = std::move(note_);
        if (decision.action == Action::Allow) decision.member_of = std::move(members_);
        return decision;
    }

private:
    std::optional<Decision> best_;
    std::vector<Deferred> deferred_;
    std::vector<std::string> members_;
    std::string note_;
};

bool in_lineage(const ProcessFacts& facts, const std::string& app_id) {
    return std::ranges::find(facts.lineage, app_id) != facts.lineage.end();
}

std::string directory_prefix(const std::string& pattern) {
    const auto normalized = normalize_path(pattern);
    const auto wildcard = normalized.find_first_of("*?");
    const auto limit = wildcard == std::string::npos ? normalized.size() : wildcard;
    const auto slash = normalized.rfind('/', limit == 0 ? 0 : limit - 1);
    return slash == std::string::npos ? std::string{} : normalized.substr(0, slash);
}

Decision grant(DecisionSource source, std::string reason, const Application& app,
               const Component& component) {
    Decision d;
    d.action = Action::Allow;
    d.source = source;
    d.reason = std::move(reason);
    d.app_id = app.id;
    d.component_id = component.id;
    d.priority = app.priority;
    return d;
}

void consider_rules(const Profile& profile, const ProcessFacts& facts, TimePoint now, Ranking& ranking) {
    for (const auto& rule : profile.rules) {
        if (!rule_is_active(rule, now)) continue;
        const bool needs_hash = std::holds_alternative<HashMatcher>(rule.matcher) && !facts.hash_resolved;
        const bool needs_publisher =
            std::holds_alternative<PublisherMatcher>(rule.matcher) && !facts.publisher_resolved;
        if (needs_hash || needs_publisher) {
            ranking.defer({rule.priority, rule.action, needs_hash, needs_publisher});
            continue;
        }
        if (!matcher_matches(rule.matcher, facts.app)) continue;
        Decision d;
        d.action = rule.action;
        d.source = DecisionSource::Rule;
        d.reason = "rule";
        d.rule_id = rule.id;
        d.priority = rule.priority;
        ranking.offer(std::move(d));
    }
}

void consider_related(const Application& app, const ProcessFacts& facts, Ranking& ranking) {
    if (app.mode != DependencyMode::Related || !in_lineage(facts, app.id) ||
        is_sensitive_tool(facts.app.path))
        return;
    const Component* signed_main = nullptr;
    const Component* located_main = nullptr;
    for (const auto& component : app.components) {
        if (component.role != ComponentRole::Main) continue;
        if (!component.publisher.empty() && !signed_main) signed_main = &component;
        if (!component.path.empty() && !located_main) located_main = &component;
    }
    if (signed_main) {
        if (!facts.publisher_resolved) {
            ranking.defer({app.priority, Action::Allow, false, true});
        } else if (iequals(facts.app.publisher, signed_main->publisher) && !facts.app.publisher.empty()) {
            ranking.offer(grant(DecisionSource::Related, "application.related.signer", app, *signed_main));
            ranking.member(app.id);
        }
        return;
    }
    if (located_main) {
        const auto root = directory_prefix(located_main->path);
        if (!root.empty() && path_matches(root + "/**", facts.app.path)) {
            ranking.offer(grant(DecisionSource::Related, "application.related.location", app, *located_main));
            ranking.member(app.id);
        }
    }
}

void consider_application(const Application& app, const ProcessFacts& facts, TimePoint now,
                          Ranking& ranking) {
    for (const auto& component : app.components) {
        if (!component_is_active(component, now)) continue;
        const bool is_main = component.role == ComponentRole::Main;
        if (!is_main && app.mode == DependencyMode::SelectedOnly) continue;
        const bool reachable = is_main || component.scope == Scope::Independent || in_lineage(facts, app.id);
        if (!reachable) continue;
        const auto state = match_component(component, facts);
        switch (state) {
            case MatchState::Exact:
            case MatchState::Updated: {
                if (state == MatchState::Updated) ranking.note("component_updated:" + component.id);
                ranking.offer(is_main ? grant(DecisionSource::Application, "application.main", app, component)
                                      : grant(DecisionSource::Dependency,
                                              component.scope == Scope::Independent
                                                  ? "application.dependency.independent"
                                                  : "application.dependency",
                                              app, component));
                ranking.member(app.id);
                break;
            }
            case MatchState::Changed:
                ranking.note("component_changed:" + component.id);
                break;
            case MatchState::NeedsHash:
                ranking.defer({app.priority, Action::Allow, true, false});
                break;
            case MatchState::NeedsPublisher:
                ranking.defer({app.priority, Action::Allow, false, true});
                break;
            case MatchState::None:
                break;
        }
    }
    consider_related(app, facts, ranking);
}

Decision protected_decision(const ProtectedHit& hit) {
    Decision d;
    d.action = Action::Allow;
    d.source = DecisionSource::Protected;
    d.reason = std::string(hit.reason);
    d.needs_publisher = hit.needs_publisher;
    d.priority = std::numeric_limits<int>::max();
    return d;
}

}  // namespace

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

bool component_is_active(const Component& component, TimePoint now) {
    return !component.expires_at || now < *component.expires_at;
}

MatchState match_component(const Component& component, const ProcessFacts& facts) {
    const bool by_package =
        !component.package_family.empty() && iequals(component.package_family, facts.app.package_family);
    const bool by_path = !component.path.empty() && path_matches(component.path, facts.app.path);
    const bool located =
        !component.package_family.empty() ? by_package && (component.path.empty() || by_path) : by_path;
    if (!located) return MatchState::None;

    if (!component.sha256.empty()) {
        if (!facts.hash_resolved) return MatchState::NeedsHash;
        if (iequals(component.sha256, facts.app.sha256)) return MatchState::Exact;
        if (!component.follow_updates || component.publisher.empty()) return MatchState::Changed;
        if (!facts.publisher_resolved) return MatchState::NeedsPublisher;
        return !facts.app.publisher.empty() && iequals(component.publisher, facts.app.publisher)
                   ? MatchState::Updated
                   : MatchState::Changed;
    }
    if (!component.publisher.empty()) {
        if (!facts.publisher_resolved) return MatchState::NeedsPublisher;
        return !facts.app.publisher.empty() && iequals(component.publisher, facts.app.publisher)
                   ? MatchState::Exact
                   : MatchState::Changed;
    }
    return MatchState::Exact;
}

Decision evaluate(const Profile& profile, const Protection& protection, const ProcessFacts& facts,
                  TimePoint now) {
    if (const auto hit = protection.classify(facts)) {
        return protected_decision(*hit);
    }
    if (facts.continuity != Continuity::None) {
        Decision d;
        d.action = Action::Allow;
        d.source = DecisionSource::Continuity;
        d.reason = facts.continuity == Continuity::Houseguest ? "houseguest" : "houseguest.webview2";
        d.priority = std::numeric_limits<int>::max();
        return d;
    }
    if (!facts.image_known) {
        Decision d;
        d.source = DecisionSource::Unresolved;
        d.reason = "identity.unresolved";
        return d;
    }

    Ranking ranking;
    consider_rules(profile, facts, now, ranking);
    for (const auto& app : profile.apps) consider_application(app, facts, now, ranking);
    auto decision = ranking.settle(profile.default_action);

    if (decision.action == Action::Deny) {
        for (const auto& app : profile.apps) {
            if (app.unknown == UnknownPolicy::Review && in_lineage(facts, app.id)) {
                decision.review_app = app.id;
                break;
            }
        }
    }
    return decision;
}

Decision evaluate(const Profile& profile, const Protection& protection, const AppIdentity& app,
                  TimePoint now) {
    ProcessFacts facts;
    facts.app = app;
    facts.hash_resolved = true;
    facts.publisher_resolved = true;
    return evaluate(profile, protection, facts, now);
}

std::string_view source_name(DecisionSource source) {
    switch (source) {
        case DecisionSource::Protected:
            return "protected";
        case DecisionSource::Continuity:
            return "continuity";
        case DecisionSource::Rule:
            return "rule";
        case DecisionSource::Application:
            return "application";
        case DecisionSource::Dependency:
            return "dependency";
        case DecisionSource::Related:
            return "related";
        case DecisionSource::Default:
            return "default";
        case DecisionSource::Unresolved:
            return "unresolved";
    }
    return "unknown";
}

SimulationReport simulate(const Profile& profile, const Protection& protection,
                          const std::vector<AppIdentity>& apps, TimePoint now) {
    SimulationReport report;
    report.apps.reserve(apps.size());
    for (const auto& app : apps) {
        auto decision = evaluate(profile, protection, app, now);
        (decision.action == Action::Allow ? report.allowed : report.denied)++;
        report.apps.push_back({app, std::move(decision)});
    }
    return report;
}

}  // namespace houseguest::core
