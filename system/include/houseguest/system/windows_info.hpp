#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"

namespace houseguest::system {

struct LocalUser {
    std::string name;
    std::string sid;
    bool disabled = false;
};

struct InstalledApp {
    std::string name;
    std::string path;
    std::string company;
};

std::vector<LocalUser> list_local_users();
std::optional<std::string> process_user_sid(std::uint32_t pid);
std::optional<std::filesystem::path> process_image_path(std::uint32_t pid);
std::vector<std::uint32_t> running_process_ids();
bool terminate_process(std::uint32_t pid);

// Returns the signer subject only when the Authenticode signature chains to a trusted root.
std::string trusted_publisher(const std::filesystem::path& file);

std::vector<InstalledApp> discover_installed_apps();

core::Json detect_capabilities();

void disconnect_sessions(const std::vector<std::string>& user_sids, bool logoff);
void notify_session(std::uint32_t session_id, const std::wstring& title, const std::wstring& message);

}  // namespace houseguest::system
