#pragma once

#include <filesystem>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::service {

// Describes one executable for people: what it declares about itself, and what its digital signature proves.
// The two are reported separately because the first is only a claim and the second is a verification. Results
// are remembered by file stamp, so a replaced or modified file is read again. Nothing here feeds a decision
// about what may run; that comes from IdentityResolver and the policy.
class FileDetails {
public:
    core::Json describe(const std::filesystem::path& file);
    [[nodiscard]] std::size_t cached() const;

    static constexpr std::size_t kEntries = 512;

private:
    struct Entry {
        system::FileStamp stamp;
        core::Json value;
        std::list<std::string>::iterator position;
    };
    mutable std::mutex mutex_;
    std::list<std::string> order_;
    std::unordered_map<std::string, Entry> cache_;
};

}  // namespace houseguest::service
