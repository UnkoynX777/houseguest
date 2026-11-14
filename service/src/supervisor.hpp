#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "houseguest/core/evaluator.hpp"
#include "houseguest/core/types.hpp"

#include "identity_resolver.hpp"
#include "process_trace.hpp"

namespace houseguest::service {

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
    bool enforced = false;
    bool audit_only = false;
};

enum class SupervisionMode { Stopped, Events, Polling };

struct SupervisorStats {
    SupervisionMode mode = SupervisionMode::Stopped;
    std::string degraded_reason;
    std::uint64_t inspected = 0;
    std::uint64_t terminated = 0;
    std::uint64_t termination_failed = 0;
    std::uint64_t pid_reuse_skipped = 0;
    std::size_t latency_samples = 0;
    double latency_average_ms = 0;
    double latency_p95_ms = 0;
    double latency_max_ms = 0;
};

class Supervisor {
public:
    using DenialHandler = std::function<void(const Denial&)>;
    using ModeHandler = std::function<void(SupervisionMode, const std::string&)>;

    Supervisor(IdentityResolver& resolver, DenialHandler on_denial, ModeHandler on_mode);
    ~Supervisor();

    bool start();
    void stop();
    void apply(std::shared_ptr<const EnforcementPolicy> policy);
    void sweep();
    [[nodiscard]] SupervisionMode mode() const { return mode_; }
    [[nodiscard]] bool running() const { return mode_ != SupervisionMode::Stopped; }
    [[nodiscard]] SupervisorStats stats() const;

private:
    struct Pending {
        std::uint32_t pid;
        std::uint64_t event_ticks;
    };

    void enqueue(std::uint32_t pid, std::uint64_t event_ticks);
    void work(const std::stop_token& stop);
    void maintain(const std::stop_token& stop);
    void inspect(const Pending& pending);
    bool claim(std::uint32_t pid, std::uint64_t created_ticks);
    void forget_exited();
    void set_mode(SupervisionMode mode, const std::string& reason);
    void record_latency(std::uint64_t event_ticks);

    IdentityResolver& resolver_;
    DenialHandler on_denial_;
    ModeHandler on_mode_;
    ProcessTrace trace_;

    std::mutex policy_mutex_;
    std::shared_ptr<const EnforcementPolicy> policy_;

    std::mutex queue_mutex_;
    std::condition_variable_any queue_ready_;
    std::queue<Pending> queue_;

    std::mutex handled_mutex_;
    std::unordered_map<std::uint32_t, std::uint64_t> handled_;

    mutable std::mutex stats_mutex_;
    std::deque<double> latencies_;
    std::string degraded_reason_;
    std::atomic<std::uint64_t> inspected_ = 0;
    std::atomic<std::uint64_t> terminated_ = 0;
    std::atomic<std::uint64_t> termination_failed_ = 0;
    std::atomic<std::uint64_t> pid_reuse_skipped_ = 0;

    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
    std::vector<std::jthread> workers_;
    std::jthread maintenance_;
    std::atomic<SupervisionMode> mode_ = SupervisionMode::Stopped;
};

}  // namespace houseguest::service
