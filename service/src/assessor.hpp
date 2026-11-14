#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include "houseguest/core/evaluator.hpp"
#include "houseguest/system/process.hpp"

#include "continuity.hpp"
#include "identity_resolver.hpp"
#include "lineage.hpp"
#include "policy.hpp"

namespace houseguest::service {

struct Assessment {
    core::ProcessFacts facts;
    core::Decision decision;
    std::filesystem::path image;
    std::uint32_t parent_pid = 0;
    std::string sid;
    std::uint32_t session = 0;
    bool degraded = false;
    const char* degraded_stage = nullptr;
    const char* retry_stage = nullptr;
    std::uint32_t error = 0;
};

// Turns a running process into the facts the engine decides on, and remembers which application a process
// that was allowed belongs to. Everything it learns is read from the operating system for that process:
// the kernel's critical and protection flags, its token, its image, the file's hash and signature, and the
// parent it reports, which only counts when a record for that very process exists.
class Assessor {
public:
    using ParentPending = std::function<bool(std::uint32_t parent_pid)>;

    Assessor(IdentityResolver& resolver, ContinuityGuard& continuity, Lineage& lineage,
             const std::string& system_root, const std::string& program_files);

    Assessment assess(const system::ProcessRef& process, std::uint64_t created, const std::string& sid,
                      const EnforcementPolicy* policy, bool final, const ParentPending& parent_pending);

    void resolve_fully(Assessment& assessment, const system::ProcessRef& process, bool with_publisher = true);

private:
    std::optional<std::uint32_t> parent_of(std::uint32_t pid, std::uint64_t created);

    IdentityResolver& resolver_;
    ContinuityGuard& continuity_;
    Lineage& lineage_;
    core::Protection protection_;
    std::mutex snapshot_mutex_;
    system::ProcessSnapshot snapshot_;
};

}  // namespace houseguest::service
