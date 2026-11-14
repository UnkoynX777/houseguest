#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "houseguest/core/evaluator.hpp"
#include "houseguest/core/types.hpp"

namespace houseguest::service {

class IdentityResolver {
public:
    core::AppIdentity resolve(const std::filesystem::path& file, bool with_hash, bool with_publisher);

private:
    struct Entry {
        std::uintmax_t size = 0;
        std::filesystem::file_time_type modified;
        std::optional<std::string> sha256;
        std::optional<std::string> publisher;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, Entry> cache_;
};

struct EnforcementPolicy {
    core::Profile profile;
    core::Baseline baseline;
    std::set<std::string> guest_sids;
    bool audit_only = false;
    bool needs_hash = false;
    bool needs_publisher = false;
    int highest_expensive_priority = std::numeric_limits<int>::min();
};

struct Denial {
    std::uint32_t pid = 0;
    std::uint32_t session_id = 0;
    std::string user_sid;
    core::AppIdentity app;
    core::Decision decision;
    bool enforced = true;
};

// Process creation is observed through ETW and offending processes are terminated afterwards.
// This is reactive: a process can execute briefly before it is stopped.
class Supervisor {
public:
    using DenialHandler = std::function<void(const Denial&)>;

    Supervisor(IdentityResolver& resolver, DenialHandler on_denial);
    ~Supervisor();

    bool start(std::string& error);
    void stop();
    void apply(std::shared_ptr<const EnforcementPolicy> policy);
    void sweep();
    [[nodiscard]] bool running() const { return running_; }

    static void on_event(void* record);

private:
    void enqueue(std::uint32_t pid);
    void work(const std::stop_token& stop);
    void inspect(std::uint32_t pid);

    IdentityResolver& resolver_;
    DenialHandler on_denial_;

    std::mutex policy_mutex_;
    std::shared_ptr<const EnforcementPolicy> policy_;

    std::mutex queue_mutex_;
    std::condition_variable_any queue_ready_;
    std::queue<std::uint32_t> queue_;
    std::vector<std::jthread> workers_;

    std::uint64_t session_handle_ = 0;
    std::uint64_t trace_handle_ = 0;
    std::thread trace_thread_;
    std::atomic<bool> running_ = false;
};

}  // namespace houseguest::service
