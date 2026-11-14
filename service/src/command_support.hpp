#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

#include "houseguest/core/serialization.hpp"
#include "houseguest/core/types.hpp"
#include "houseguest/version.hpp"

namespace houseguest::service {

struct Failure : std::runtime_error {
    Failure(std::string error_code, const std::string& message, core::Json extra = core::Json::object())
        : std::runtime_error(message), code(std::move(error_code)), details(std::move(extra)) {}
    std::string code;
    core::Json details;
};

std::int64_t now_seconds();
std::string require_text(const core::Json& args, const char* key);
std::int64_t require_int(const core::Json& args, const char* key, std::int64_t low, std::int64_t high);
std::string random_id(const char* prefix);
bool valid_expiry_action(const std::string& action);
core::Json action_name(core::Action action);

}  // namespace houseguest::service
