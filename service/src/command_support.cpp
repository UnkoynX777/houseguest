#include "command_support.hpp"

#include <chrono>

#include "houseguest/system/crypto.hpp"

namespace houseguest::service {

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string require_text(const core::Json& args, const char* key) {
    const auto it = args.find(key);
    if (it == args.end() || !it->is_string())
        throw Failure("bad_request", std::string("missing text field: ") + key);
    return it->get<std::string>();
}

std::int64_t require_int(const core::Json& args, const char* key, std::int64_t low, std::int64_t high) {
    const auto it = args.find(key);
    if (it == args.end() || !it->is_number_integer())
        throw Failure("bad_request", std::string("missing integer field: ") + key);
    const auto value = it->get<std::int64_t>();
    if (value < low || value > high) throw Failure("bad_request", std::string("value out of range: ") + key);
    return value;
}

std::string random_id(const char* prefix) {
    return std::string(prefix) + system::to_hex(system::random_bytes(6));
}

bool valid_expiry_action(const std::string& action) {
    return action == "none" || action == "disconnect" || action == "logoff" || action == "disable";
}

core::Json action_name(core::Action action) {
    return action == core::Action::Allow ? "allow" : "deny";
}

}  // namespace houseguest::service
