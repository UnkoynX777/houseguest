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

std::vector<LocalUser> list_local_users();
// True only for a drive-letter path on a fixed, removable, optical or RAM disk. Network locations return
// false so the privileged service never opens files that an unprivileged user can point at a remote server.
bool is_local_disk_path(const std::filesystem::path& file);
// Returns the signer subject only when the Authenticode signature chains to a trusted root.
std::string trusted_publisher(const std::filesystem::path& file);

// True when the directory's owner or a grant in its ACL lets one of the given accounts, or a broad group
// such as Users or Everyone, add files or change the directory. Used to decide whether a folder can be
// trusted to hold Houseguest's own files.
bool directory_writable_by(const std::filesystem::path& directory, const std::vector<std::string>& sids);

struct SessionActionResult {
    bool complete = true;
    unsigned affected = 0;
};

SessionActionResult disconnect_sessions(const std::vector<std::string>& user_sids, bool logoff);
void notify_session(std::uint32_t session_id, const std::wstring& title, const std::wstring& message);

}  // namespace houseguest::system
