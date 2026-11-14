#pragma once

#include <nlohmann/json.hpp>

#include "houseguest/core/types.hpp"

namespace houseguest::core {

using Json = nlohmann::json;

Json to_json(const Rule& rule);
Json to_json(const Profile& profile);
Json to_json(const AppIdentity& app);

Rule rule_from_json(const Json& json);
Profile profile_from_json(const Json& json);
AppIdentity app_identity_from_json(const Json& json);

std::vector<Profile> builtin_profiles();

}  // namespace houseguest::core
