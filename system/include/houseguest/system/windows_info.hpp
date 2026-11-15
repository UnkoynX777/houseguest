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

// What the file system says about one file at one moment. The change time is maintained by the file system and
// cannot be set from user mode, unlike the write time, and the file id names the file itself, so a file
// replaced under the same name, size and write time still yields a different stamp.
struct FileStamp {
    std::uint64_t size = 0;
    std::int64_t written = 0;
    std::int64_t changed = 0;
    std::uint64_t volume = 0;
    std::uint64_t id_low = 0;
    std::uint64_t id_high = 0;

    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};

[[nodiscard]] std::optional<FileStamp> file_stamp(const std::filesystem::path& file);
// What the executable's version resource declares about itself. These are claims made by whoever built the file,
// not facts: anything can write any company name here, so none of it is evidence of who the publisher is.
struct FileMetadata {
    std::string company;
    std::string product;
    std::string description;
    std::string file_version;
    std::string product_version;
    std::string copyright;
    std::string original_name;
    std::string internal_name;
};
[[nodiscard]] FileMetadata read_file_metadata(const std::filesystem::path& file);

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
