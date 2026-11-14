#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/crypto.hpp"

namespace houseguest::service {

struct AuditReport {
    bool valid = true;
    bool truncated = false;
    std::uint64_t entries = 0;
    std::uint64_t last_sequence = 0;
    std::string problem;
};

class AuditLog {
public:
    AuditLog(std::filesystem::path file, system::Bytes key);

    void append(const std::string& actor, const std::string& event, core::Json detail = core::Json::object());
    core::Json tail(std::size_t limit) const;

    struct Query {
        std::size_t limit = 50;
        std::uint64_t before = 0;
        std::vector<std::string> events;
        std::string text;
        std::int64_t since = 0;
    };
    core::Json query(const Query& query) const;
    [[nodiscard]] AuditReport verify() const;
    [[nodiscard]] bool verify_chain() const { return verify().valid; }
    [[nodiscard]] std::uint64_t write_failures() const;
    [[nodiscard]] std::uint64_t sequence() const {
        std::scoped_lock lock(mutex_);
        return sequence_;
    }

    static constexpr std::uintmax_t kMaxFileBytes = 2 * 1024 * 1024;
    static constexpr int kRotatedFiles = 8;

private:
    [[nodiscard]] std::vector<std::filesystem::path> files_oldest_first() const;
    [[nodiscard]] std::filesystem::path rotated(int index) const;
    [[nodiscard]] std::filesystem::path anchor_file() const;
    [[nodiscard]] std::string seal(int version, const std::string& previous, const core::Json& entry) const;
    void recover();
    void rotate_if_needed();
    void write_anchor();
    bool append_line(const std::string& line);

    std::filesystem::path file_;
    system::Bytes key_;
    mutable std::mutex mutex_;
    std::string last_hash_;
    std::uint64_t sequence_ = 0;
    std::uint64_t write_failures_ = 0;
    bool truncation_found_ = false;
};

}  // namespace houseguest::service
