#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "houseguest/core/protection.hpp"
#include "houseguest/core/types.hpp"

namespace houseguest::core {

enum class MatchState { None, Exact, Updated, Changed, NeedsHash, NeedsPublisher };

[[nodiscard]] bool matcher_matches(const Matcher& matcher, const AppIdentity& app);

[[nodiscard]] bool rule_is_active(const Rule& rule, TimePoint now);

[[nodiscard]] bool component_is_active(const Component& component, TimePoint now);

// How a running file relates to a component's pinned identity. Updated means the hash changed but the
// pinned signer still vouches for it and the component opted in to following updates.
[[nodiscard]] MatchState match_component(const Component& component, const ProcessFacts& facts);

// Evaluation order, each step final unless noted:
//   1. Protection: kernel-critical, protected processes and the audited Windows table.
//   2. Continuity: Houseguest's own verified processes and the WebView2 processes they started.
//   3. Candidates, ranked by priority, then Deny over Allow, then source (rule, application, dependency,
//      related), then id: profile rules, application executables, approved components, related descendants.
//   4. The profile default.
// A decision that needs a hash or signature that has not been resolved yet says so instead of guessing.
[[nodiscard]] Decision evaluate(const Profile& profile, const Protection& protection,
                                const ProcessFacts& facts, TimePoint now);

[[nodiscard]] Decision evaluate(const Profile& profile, const Protection& protection, const AppIdentity& app,
                                TimePoint now);

[[nodiscard]] std::string_view source_name(DecisionSource source);

struct SimulatedApp {
    AppIdentity app;
    Decision decision;
};

struct SimulationReport {
    std::vector<SimulatedApp> apps;
    std::size_t allowed = 0;
    std::size_t denied = 0;
};

[[nodiscard]] SimulationReport simulate(const Profile& profile, const Protection& protection,
                                        const std::vector<AppIdentity>& apps, TimePoint now);

}  // namespace houseguest::core
