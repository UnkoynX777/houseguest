#pragma once

#include <string>
#include <string_view>

namespace houseguest::core {

[[nodiscard]] std::string normalize_path(std::string_view path);

[[nodiscard]] bool is_valid_path_pattern(std::string_view pattern);

[[nodiscard]] bool path_matches(std::string_view pattern, std::string_view path);

}  // namespace houseguest::core
