#pragma once

#include <string>
#include <vector>

#include "houseguest/core/evaluator.hpp"
#include "houseguest/core/types.hpp"

namespace houseguest::core {

enum class Severity { Info, Warning, Error };

enum class FindingCode {
    InvalidMatcher,
    DuplicateRuleId,
    ConflictingRules,
    ShadowedRule,
    ExpiredRule,
    BaselineProtected,
    NoAllowances,
    InvalidComponent,
    DuplicateComponentId,
    ApplicationWithoutMain,
    WeakIndependent,
    UnpinnedComponent,
    SensitiveComponent,
    ApplicationBlockedByRule,
    RelatedWithoutSigner,
    ExpiredComponent,
};

struct Finding {
    Severity severity;
    FindingCode code;
    std::vector<std::string> rule_ids;
    std::string message;
};

// Findings about applications and components list the application id first, then the component id.
[[nodiscard]] std::vector<Finding> analyze(const Profile& profile, const Protection& protection,
                                           TimePoint now);

[[nodiscard]] bool has_errors(const std::vector<Finding>& findings);

}  // namespace houseguest::core
