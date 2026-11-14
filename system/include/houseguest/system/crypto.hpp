#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace houseguest::system {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

Bytes random_bytes(std::size_t count);
Bytes sha256(ByteView data);
std::optional<std::string> sha256_file(const std::filesystem::path& file);
Bytes hmac_sha256(ByteView key, ByteView data);
Bytes pbkdf2_sha256(std::string_view password, ByteView salt, std::uint32_t iterations, std::size_t length);

std::string to_hex(ByteView data);
std::optional<Bytes> from_hex(std::string_view hex);
bool constant_time_equal(ByteView a, ByteView b);

Bytes dpapi_protect_machine(ByteView data);
std::optional<Bytes> dpapi_unprotect(ByteView data);

}  // namespace houseguest::system
