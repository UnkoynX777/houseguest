#pragma once

#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"

namespace houseguest::system {

enum class Health { Ok, Degraded, Missing };

struct CapabilityCheck {
    std::string id;
    Health health = Health::Ok;
    bool critical = false;
    std::string detail;
};

struct Capabilities {
    std::string product_name;
    std::string edition;
    std::string display_version;
    std::string build;
    bool supported_build = false;
    std::vector<CapabilityCheck> checks;

    [[nodiscard]] bool critical_failure() const;
};

Capabilities probe_capabilities();
core::Json to_json(const Capabilities& capabilities);

}  // namespace houseguest::system
