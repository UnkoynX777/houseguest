#pragma once

#include <cstdint>
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
    std::uint64_t revision = 0;
};

}  // namespace houseguest::service
