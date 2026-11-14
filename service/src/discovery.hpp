#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/core/types.hpp"

namespace houseguest::service {

inline constexpr const char* kObservedMember = "$observed";

struct Observation {
    core::AppIdentity identity;
    std::string parent_path;
    bool root = false;
    bool descendant = false;
    bool windows_protected = false;
};

// Watches one application run for a limited time and lists the programs that took part, so the owner can
// approve the ones the application really needs. It records, never grants. Every item carries the evidence
// the owner needs to judge it, and nothing is approved without an explicit choice.
class Discovery {
public:
    struct Target {
        std::string label;
        std::string path;
        std::string package_family;
        std::string user_sid;
        std::int64_t minutes = 10;
    };

    void start(Target target);
    void stop();
    [[nodiscard]] bool active();
    [[nodiscard]] bool wants(const std::string& sid);
    [[nodiscard]] bool is_root(const core::AppIdentity& identity);
    [[nodiscard]] bool related(const core::AppIdentity& identity);
    void note(const Observation& observation);
    void complete(const core::AppIdentity& identity);
    [[nodiscard]] core::Json report();

    static constexpr std::size_t kMaxItems = 400;

private:
    struct Item {
        core::AppIdentity identity;
        std::string parent_path;
        std::uint32_t runs = 0;
        std::int64_t first_ms = 0;
        bool root = false;
        bool descendant = false;
        bool windows_protected = false;
    };

    void expire_locked();
    [[nodiscard]] core::Json classify(const Item& item, const std::string& root_directory,
                                      const std::string& root_publisher) const;

    std::mutex mutex_;
    bool running_ = false;
    Target target_;
    std::string root_directory_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point ends_;
    std::int64_t ends_at_ = 0;
    std::map<std::string, Item> items_;
    std::uint64_t dropped_ = 0;
};

}  // namespace houseguest::service
