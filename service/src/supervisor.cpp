#include "supervisor.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>

#include "houseguest/core/glob.hpp"
#include "houseguest/system/process.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

constexpr unsigned kWorkerCount = 2;
constexpr std::size_t kMaxQueue = 8192;
constexpr std::size_t kLatencyWindow = 512;
constexpr std::uint64_t kTicksPerMillisecond = 10000;
constexpr std::uint64_t kReuseTolerance = 50 * kTicksPerMillisecond;
constexpr auto kEventSweepInterval = std::chrono::seconds(5);
constexpr auto kPollingSweepInterval = std::chrono::seconds(1);
constexpr auto kRestartInterval = std::chrono::seconds(15);

bool decided_by_path(const EnforcementPolicy& policy, const core::Decision& decision) {
    if (decision.source == core::DecisionSource::Baseline) return true;
    if (decision.source != core::DecisionSource::Rule) return false;
    return decision.priority > policy.highest_expensive_priority ||
           (decision.priority == policy.highest_expensive_priority && decision.action == core::Action::Deny);
}

}  // namespace

Supervisor::Supervisor(IdentityResolver& resolver, DenialHandler on_denial, ModeHandler on_mode)
    : resolver_(resolver),
      on_denial_(std::move(on_denial)),
      on_mode_(std::move(on_mode)),
      trace_([this](std::uint32_t pid, std::uint64_t ticks) { enqueue(pid, ticks); }) {}

Supervisor::~Supervisor() {
    stop();
}

bool Supervisor::start() {
    if (running()) return true;
    std::string error;
    if (trace_.start(error)) {
        set_mode(SupervisionMode::Events, "");
    } else {
        set_mode(SupervisionMode::Polling, error);
    }
    for (unsigned i = 0; i < kWorkerCount; ++i) {
        workers_.emplace_back([this](const std::stop_token& stop) { work(stop); });
    }
    maintenance_ = std::jthread([this](const std::stop_token& stop) { maintain(stop); });
    return true;
}

void Supervisor::stop() {
    if (mode_.exchange(SupervisionMode::Stopped) == SupervisionMode::Stopped) return;
    trace_.stop();
    maintenance_.request_stop();
    for (auto& worker : workers_) worker.request_stop();
    wake_.notify_all();
    queue_ready_.notify_all();
    maintenance_ = {};
    workers_.clear();
}

void Supervisor::apply(std::shared_ptr<const EnforcementPolicy> policy) {
    {
        std::scoped_lock lock(policy_mutex_);
        policy_ = std::move(policy);
    }
    std::scoped_lock lock(handled_mutex_);
    handled_.clear();
}

void Supervisor::sweep() {
    for (const auto pid : system::running_process_ids()) {
        if (pid > 4) enqueue(pid, 0);
    }
}

SupervisorStats Supervisor::stats() const {
    SupervisorStats stats;
    stats.mode = mode_;
    stats.inspected = inspected_;
    stats.terminated = terminated_;
    stats.termination_failed = termination_failed_;
    stats.pid_reuse_skipped = pid_reuse_skipped_;
    std::scoped_lock lock(stats_mutex_);
    stats.degraded_reason = degraded_reason_;
    stats.latency_samples = latencies_.size();
    if (!latencies_.empty()) {
        std::vector<double> sorted(latencies_.begin(), latencies_.end());
        std::ranges::sort(sorted);
        double total = 0;
        for (const double value : sorted) total += value;
        stats.latency_average_ms = total / static_cast<double>(sorted.size());
        stats.latency_p95_ms = sorted[(sorted.size() - 1) * 95 / 100];
        stats.latency_max_ms = sorted.back();
    }
    return stats;
}

void Supervisor::enqueue(std::uint32_t pid, std::uint64_t event_ticks) {
    {
        std::scoped_lock lock(queue_mutex_);
        if (queue_.size() >= kMaxQueue) return;
        queue_.push({pid, event_ticks});
    }
    queue_ready_.notify_one();
}

void Supervisor::set_mode(SupervisionMode mode, const std::string& reason) {
    const auto previous = mode_.exchange(mode);
    {
        std::scoped_lock lock(stats_mutex_);
        degraded_reason_ = reason;
    }
    if (previous != mode && on_mode_) on_mode_(mode, reason);
}

void Supervisor::work(const std::stop_token& stop) {
    for (;;) {
        Pending pending{};
        {
            std::unique_lock lock(queue_mutex_);
            if (!queue_ready_.wait(lock, stop, [this] { return !queue_.empty(); })) return;
            pending = queue_.front();
            queue_.pop();
        }
        try {
            inspect(pending);
        } catch (const std::exception&) {
        }
    }
}

void Supervisor::maintain(const std::stop_token& stop) {
    auto last_restart = std::chrono::steady_clock::now();
    std::unique_lock lock(wake_mutex_);
    while (!stop.stop_requested()) {
        const auto interval = mode_ == SupervisionMode::Events ? kEventSweepInterval : kPollingSweepInterval;
        wake_.wait_for(lock, stop, interval, [] { return false; });
        if (stop.stop_requested() || mode_ == SupervisionMode::Stopped) return;

        if (mode_ == SupervisionMode::Events && !trace_.alive()) {
            set_mode(SupervisionMode::Polling, "The process event session ended unexpectedly.");
            last_restart = std::chrono::steady_clock::now();
        } else if (mode_ == SupervisionMode::Polling &&
                   std::chrono::steady_clock::now() - last_restart >= kRestartInterval) {
            last_restart = std::chrono::steady_clock::now();
            std::string error;
            if (trace_.start(error)) set_mode(SupervisionMode::Events, "");
        }

        forget_exited();
        sweep();
    }
}

bool Supervisor::claim(std::uint32_t pid, std::uint64_t created_ticks) {
    std::scoped_lock lock(handled_mutex_);
    auto [it, inserted] = handled_.try_emplace(pid, created_ticks);
    if (inserted) return true;
    if (it->second == created_ticks) return false;
    it->second = created_ticks;
    return true;
}

void Supervisor::forget_exited() {
    const auto alive = system::running_process_ids();
    const std::unordered_set<std::uint32_t> present(alive.begin(), alive.end());
    std::scoped_lock lock(handled_mutex_);
    std::erase_if(handled_, [&](const auto& entry) { return !present.contains(entry.first); });
}

void Supervisor::record_latency(std::uint64_t event_ticks) {
    const auto now = system::ProcessRef::now_ticks();
    if (event_ticks == 0 || now < event_ticks) return;
    std::scoped_lock lock(stats_mutex_);
    latencies_.push_back(static_cast<double>(now - event_ticks) / static_cast<double>(kTicksPerMillisecond));
    if (latencies_.size() > kLatencyWindow) latencies_.pop_front();
}

void Supervisor::inspect(const Pending& pending) {
    std::shared_ptr<const EnforcementPolicy> policy;
    {
        std::scoped_lock lock(policy_mutex_);
        policy = policy_;
    }
    if (!policy || pending.pid <= 4 || pending.pid == GetCurrentProcessId()) return;

    const auto process = system::ProcessRef::open(pending.pid);
    if (!process) return;
    const auto created = process->creation_ticks();
    if (!created) return;
    if (pending.event_ticks != 0 && *created > pending.event_ticks + kReuseTolerance) {
        ++pid_reuse_skipped_;
        return;
    }
    if (!claim(pending.pid, *created)) return;
    ++inspected_;

    const auto sid = process->user_sid();
    if (!sid || !policy->guest_sids.contains(*sid)) return;
    const auto image = process->image_path();
    if (!image) return;

    const auto now = std::chrono::system_clock::now();
    auto identity = resolver_.resolve(*image, false, false);
    auto decision = core::evaluate(policy->profile, policy->baseline, identity, now);
    if (!decided_by_path(*policy, decision) && (policy->needs_hash || policy->needs_publisher)) {
        identity = resolver_.resolve(*image, policy->needs_hash, policy->needs_publisher);
        decision = core::evaluate(policy->profile, policy->baseline, identity, now);
    }
    if (decision.action == core::Action::Allow) return;

    bool terminated = false;
    if (!policy->audit_only) {
        terminated = process->terminate();
        ++(terminated ? terminated_ : termination_failed_);
        if (terminated) record_latency(pending.event_ticks);
    }
    on_denial_(
        {pending.pid, process->session_id(), *sid, identity, decision, terminated, policy->audit_only});
}

}  // namespace houseguest::service
