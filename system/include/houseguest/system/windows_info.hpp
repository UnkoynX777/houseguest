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
// Returns the signer subject only when the Authenticode signature chains to a trusted root.
std::string trusted_publisher(const std::filesystem::path& file);

std::vector<InstalledApp> discover_installed_apps();

void disconnect_sessions(const std::vector<std::string>& user_sids, bool logoff);
void notify_session(std::uint32_t session_id, const std::wstring& title, const std::wstring& message);

}  // namespace houseguest::system
