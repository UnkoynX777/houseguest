#pragma once

#include <set>
#include <string>

#include "houseguest/core/evaluator.hpp"
#include "houseguest/core/types.hpp"

namespace houseguest::service {

struct EnforcementPolicy {
    core::Profile profile;
    core::Protection protection;
    std::set<std::string> guest_sids;
    std::string profile_id;
    std::string profile_name;
    bool audit_only = false;
};

}  // namespace houseguest::service
