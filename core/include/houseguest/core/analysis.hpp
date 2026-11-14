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
};

struct Finding {
    Severity severity;
    FindingCode code;
    std::vector<std::string> rule_ids;
    std::string message;
};

[[nodiscard]] std::vector<Finding> analyze(const Profile& profile, const Baseline& baseline, TimePoint now);

[[nodiscard]] bool has_errors(const std::vector<Finding>& findings);

}  // namespace houseguest::core
