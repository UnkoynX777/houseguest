#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "api.hpp"
#include "tunnel.hpp"

namespace houseguest::service::cloudflare {

// Starts cloudflared in a job object that ends it if the service dies, with no console window and its output
// delivered line by line. Secrets are never passed on the command line or in the environment.
[[nodiscard]] Launcher windows_launcher();

// True when the file carries a valid Authenticode signature issued to Cloudflare.
[[nodiscard]] bool is_official_cloudflared(const std::filesystem::path& file, std::string* detail = nullptr);

[[nodiscard]] std::optional<std::filesystem::path> locate_cloudflared(
    const std::vector<std::filesystem::path>& folders);

// Downloads the official Windows build over HTTPS, checks its signature and installs it atomically. The file
// is discarded unless the signature is valid and names Cloudflare.
bool install_cloudflared(const Transport& transport, const std::filesystem::path& folder, std::string& error);

// Gives a folder a DACL that admits only SYSTEM and administrators.
bool restrict_to_administrators(const std::filesystem::path& folder);

}  // namespace houseguest::service::cloudflare
