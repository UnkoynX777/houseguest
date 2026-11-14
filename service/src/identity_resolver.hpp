#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "houseguest/core/evaluator.hpp"

namespace houseguest::service {

struct Resolution {
    core::AppIdentity identity;
    bool complete = true;
};

class IdentityResolver {
public:
    core::AppIdentity resolve(const std::filesystem::path& file, bool with_hash, bool with_publisher);
    Resolution resolve_checked(const std::filesystem::path& file, bool with_hash, bool with_publisher,
                               const std::string& package_family = {});
    [[nodiscard]] std::size_t cached() const;

private:
    struct Entry {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified;
        std::optional<std::string> sha256;
        std::optional<std::string> publisher;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> cache_;
};

}  // namespace houseguest::service
