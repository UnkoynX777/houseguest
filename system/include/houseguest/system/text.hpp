#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace houseguest::system {

std::string to_utf8(std::wstring_view wide);
std::wstring to_wide(std::string_view utf8);
std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(std::string_view utf8);

std::string expand_known_folders(std::string_view pattern);

}  // namespace houseguest::system
