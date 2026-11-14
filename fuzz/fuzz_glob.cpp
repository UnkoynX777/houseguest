#include <cstddef>
#include <cstdint>
#include <string_view>

#include "houseguest/core/glob.hpp"

using namespace houseguest::core;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto split = text.find('\n');
    const auto pattern = text.substr(0, split);
    const auto path = split == std::string_view::npos ? std::string_view{} : text.substr(split + 1);
    if (is_valid_path_pattern(pattern)) static_cast<void>(path_matches(pattern, path));
    static_cast<void>(normalize_path(path));
    return 0;
}
