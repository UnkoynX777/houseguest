#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace houseguest::system {

struct CreatedUser {
    std::string name;
    std::string sid;
};

[[nodiscard]] bool valid_managed_name(const std::string& name);

std::optional<CreatedUser> create_standard_user(const std::string& name, const std::string& password,
                                                std::string& error);
bool set_user_disabled(const std::string& sid, bool disabled);
bool set_user_expiry(const std::string& sid, std::int64_t unix_seconds);
bool delete_user(const std::string& sid);
bool user_exists(const std::string& sid);

}  // namespace houseguest::system
