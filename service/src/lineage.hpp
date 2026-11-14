#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "houseguest/system/process.hpp"

namespace houseguest::service {

inline constexpr const char* kHouseguestMember = "$houseguest";

struct ParentMembership {
    std::vector<std::string> members;
    std::string sid;
    std::uint32_t session = 0;
};

// Remembers which allowed processes belong to an application. Every record keeps a handle to its process, so
// the operating system cannot hand that process id to another program while the record exists. That makes the
// parent id a child reports unambiguous: if there is a record for it, it is that process.
class Lineage {
public:
    void remember(const system::ProcessRef& process, std::uint64_t created, const std::string& sid,
                  std::vector<std::string> members);
    [[nodiscard]] std::optional<ParentMembership> find(std::uint32_t parent_pid,
                                                       std::uint64_t child_created) const;
    void sweep();
    void clear();
    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::uint64_t saturated() const;

    static constexpr std::size_t kMaxRecords = 2048;
    static constexpr auto kGrace = std::chrono::seconds(120);

private:
    struct Record {
        system::ProcessRef process;
        std::uint64_t created = 0;
        std::string sid;
        std::vector<std::string> members;
        std::optional<std::chrono::steady_clock::time_point> gone;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Record> records_;
    std::uint64_t saturated_ = 0;
};

}  // namespace houseguest::service
