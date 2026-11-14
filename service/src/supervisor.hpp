#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "houseguest/core/types.hpp"

#include "assessor.hpp"
#include "discovery.hpp"
#include "policy.hpp"
#include "process_trace.hpp"

namespace houseguest::service {

struct Denial {
    std::uint32_t pid = 0;
    std::uint32_t session_id = 0;
    std::string user_sid;
    core::AppIdentity app;
    core::Decision decision;
    std::string parent_path;
    std::string profile_id;
    std::string profile_name;
    bool enforced = false;
    bool audit_only = false;
    bool spared = false;
};

struct InspectionFailure {
    std::uint32_t pid = 0;
    std::string stage;
    std::uint32_t error = 0;
    unsigned attempts = 0;
    bool degraded = false;
};

enum class SupervisionMode { Stopped, Events, Polling };

struct SupervisorStats {
    SupervisionMode mode = SupervisionMode::Stopped;
    std::string degraded_reason;
    std::uint64_t inspected = 0;
    std::uint64_t terminated = 0;
    std::uint64_t termination_failed = 0;
    std::uint64_t pid_reuse_skipped = 0;
    std::uint64_t retried = 0;
    std::uint64_t inspection_failed = 0;
    std::uint64_t inspection_degraded = 0;
    std::uint64_t events_dropped = 0;
    std::uint64_t reconciliations = 0;
    std::uint64_t unreadable = 0;
    std::uint64_t protection_spared = 0;
    std::uint64_t lineage_saturated = 0;
    std::size_t lineage_records = 0;
    std::size_t queue_depth = 0;
    std::size_t queue_high_water = 0;
    std::size_t retries_waiting = 0;
    std::size_t tracked = 0;
    std::size_t latency_samples = 0;
    double latency_average_ms = 0;
    double latency_p95_ms = 0;
    double latency_max_ms = 0;
    std::size_t assessment_samples = 0;
    double assessment_average_us = 0;
    double assessment_p95_us = 0;
    double assessment_max_us = 0;
};

class Supervisor {
public:
    using DenialHandler = std::function<void(const Denial&)>;
    using ModeHandler = std::function<void(SupervisionMode, const std::string&)>;
    using FailureHandler = std::function<void(const InspectionFailure&)>;
    using Clock = std::chrono::steady_clock;

    Supervisor(Assessor& assessor, Lineage& lineage, Discovery& discovery, DenialHandler on_denial,
               ModeHandler on_mode, FailureHandler on_failure);
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
        std::uint32_t pid = 0;
        std::uint64_t event_ticks = 0;
        unsigned attempt = 0;
        unsigned waits = 0;
        bool waiting_for_parent = false;
        std::uint32_t wait_parent = 0;
    };
    struct Delayed {
        Clock::time_point due;
        Pending pending;
    };
    enum class State { InFlight, Retrying, Settled };
    struct Tracked {
        std::uint64_t created = 0;
        State state = State::InFlight;
    };
    enum class Outcome { Settled, Retry };

    void enqueue(std::uint32_t pid, std::uint64_t event_ticks, bool from_event);
    void work(const std::stop_token& stop);
    void maintain(const std::stop_token& stop);
    void promote_due_retries();
    void schedule_retry(Pending pending);
    Outcome inspect(Pending& pending);
    Outcome retry_or_fail(const Pending& pending, const char* stage, std::uint32_t error, bool final);
    bool claim(std::uint32_t pid, std::uint64_t created_ticks, bool retrying);
    void mark(std::uint32_t pid, std::uint64_t created_ticks, State state);
    void forget_exited();
    bool known(std::uint32_t pid);
    bool in_flight(std::uint32_t pid, std::uint32_t except);
    bool relevant_to_observation(std::uint32_t pid);
    void finished(std::uint32_t pid);
    void wake_waiters(std::uint32_t pid);
    void observe(const system::ProcessRef& process, Assessment& assessment, std::uint64_t created);
    void record_assessment(std::chrono::steady_clock::duration elapsed);
    void set_mode(SupervisionMode mode, const std::string& reason);
    void record_latency(std::uint64_t event_ticks);

    Assessor& assessor_;
    Lineage& lineage_;
    Discovery& discovery_;
    DenialHandler on_denial_;
    ModeHandler on_mode_;
    FailureHandler on_failure_;
    ProcessTrace trace_;

    std::mutex policy_mutex_;
    std::shared_ptr<const EnforcementPolicy> policy_;

    mutable std::mutex queue_mutex_;
    std::condition_variable_any queue_ready_;
    std::deque<Pending> events_;
    std::deque<Pending> sweeps_;
    std::unordered_map<std::uint32_t, int> active_;
    std::size_t high_water_ = 0;

    mutable std::mutex retry_mutex_;
    std::vector<Delayed> retries_;

    mutable std::mutex tracked_mutex_;
    std::unordered_map<std::uint32_t, Tracked> tracked_;

    mutable std::mutex stats_mutex_;
    std::deque<double> latencies_;
    std::deque<double> assessments_;
    std::string degraded_reason_;
    std::atomic<std::uint64_t> inspected_ = 0;
    std::atomic<std::uint64_t> terminated_ = 0;
    std::atomic<std::uint64_t> termination_failed_ = 0;
    std::atomic<std::uint64_t> pid_reuse_skipped_ = 0;
    std::atomic<std::uint64_t> retried_ = 0;
    std::atomic<std::uint64_t> inspection_failed_ = 0;
    std::atomic<std::uint64_t> inspection_degraded_ = 0;
    std::atomic<std::uint64_t> events_dropped_ = 0;
    std::atomic<std::uint64_t> reconciliations_ = 0;
    std::atomic<std::uint64_t> unreadable_ = 0;
    std::atomic<std::uint64_t> spared_ = 0;
    std::atomic<bool> reconcile_requested_ = false;
    std::atomic<bool> retries_changed_ = false;

    std::mutex wake_mutex_;
    std::condition_variable_any wake_;
    std::vector<std::jthread> workers_;
    std::jthread maintenance_;
    std::atomic<SupervisionMode> mode_ = SupervisionMode::Stopped;
};

}  // namespace houseguest::service
