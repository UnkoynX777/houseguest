#pragma once

#include <string>
#include <vector>

#include "houseguest/core/types.hpp"

namespace houseguest::core {

// Programs the system needs to stay usable and recoverable. No rule can deny these.
// Intentionally excludes interpreters and admin tools (cmd, powershell, regedit, mmc):
// restricting those is a legitimate profile decision.
struct Baseline {
    std::vector<std::string> path_patterns;

    [[nodiscard]] static Baseline windows_default(const std::string& system_root,
                                                  const std::string& install_dir);
};

[[nodiscard]] bool matcher_matches(const Matcher& matcher, const AppIdentity& app);

[[nodiscard]] bool rule_is_active(const Rule& rule, TimePoint now);

[[nodiscard]] Decision evaluate(const Profile& profile, const Baseline& baseline, const AppIdentity& app,
                                TimePoint now);

struct SimulatedApp {
    AppIdentity app;
    Decision decision;
};

struct SimulationReport {
    std::vector<SimulatedApp> apps;
    std::size_t allowed = 0;
    std::size_t denied = 0;
};

[[nodiscard]] SimulationReport simulate(const Profile& profile, const Baseline& baseline,
                                        const std::vector<AppIdentity>& apps, TimePoint now);

}  // namespace houseguest::core
