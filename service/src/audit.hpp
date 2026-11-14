#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include "houseguest/core/serialization.hpp"

namespace houseguest::service {

class AuditLog {
public:
    explicit AuditLog(std::filesystem::path file);

    void append(const std::string& actor, const std::string& event, core::Json detail = core::Json::object());
    core::Json tail(std::size_t limit) const;
    bool verify_chain() const;

private:
    std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::string last_hash_;
    std::uint64_t sequence_ = 0;
};

}  // namespace houseguest::service
