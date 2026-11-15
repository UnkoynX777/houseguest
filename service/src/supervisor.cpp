#include "supervisor.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <unordered_set>

#include "houseguest/system/process.hpp"
#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

constexpr std::size_t kMaxQueue = 8192;
constexpr std::size_t kLatencyWindow = 512;
constexpr std::uint64_t kTicksPerMillisecond = 10000;
constexpr std::uint64_t kReuseTolerance = 50 * kTicksPerMillisecond;
constexpr auto kEventSweepInterval = seconds(5);
constexpr auto kPollingSweepInterval = seconds(1);
constexpr auto kRestartInterval = seconds(15);
constexpr unsigned kMaxAttempts = 7;
constexpr unsigned kMaxParentWaits = 80;
constexpr auto kParentWait = milliseconds(150);
constexpr std::array<milliseconds, 6> kBackoff = {milliseconds(25),  milliseconds(50),  milliseconds(100),
                                                  milliseconds(250), milliseconds(500), milliseconds(1000)};

unsigned worker_count() {
    const unsigned cores = std::thread::hardware_concurrency();
    return std::clamp(cores / 2, 2u, 4u);
}

bool is_service_account(const std::string& sid) {
    return sid == "S-1-5-18" || sid == "S-1-5-19" || sid == "S-1-5-20";
}

}  // namespace

Supervisor::Supervisor(Assessor& assessor, Lineage& lineage, Discovery& discovery, DenialHandler on_denial,
                       ModeHandler on_mode, FailureHandler on_failure)
    : assessor_(assessor),
      lineage_(lineage),
      discovery_(discovery),
      on_denial_(std::move(on_denial)),
      on_mode_(std::move(on_mode)),
      on_failure_(std::move(on_failure)),
      trace_([this](std::uint32_t pid, std::uint64_t ticks) { enqueue(pid, ticks, true); }) {}

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
    for (unsigned i = 0; i < worker_count(); ++i) {
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
    {
        std::scoped_lock lock(retry_mutex_);
        retries_.clear();
    }
    {
        std::scoped_lock lock(queue_mutex_);
        active_.clear();
        for (const auto& pending : events_) ++active_[pending.pid];
        for (const auto& pending : sweeps_) ++active_[pending.pid];
    }
    if (!policy_) lineage_.clear();
    std::scoped_lock lock(tracked_mutex_);
    tracked_.clear();
}

bool Supervisor::known(std::uint32_t pid) {
    std::scoped_lock lock(tracked_mutex_);
    return tracked_.contains(pid);
}

void Supervisor::sweep() {
    std::vector<std::pair<std::uint64_t, std::uint32_t>> unseen;
    for (const auto pid : system::running_process_ids()) {
        if (pid <= 4 || known(pid)) continue;
        const auto process = system::ProcessRef::open(pid);
        unseen.emplace_back(process ? process->creation_ticks().value_or(0) : 0, pid);
    }
    std::ranges::sort(unseen);
    for (const auto& [created, pid] : unseen) enqueue(pid, 0, false);
}

SupervisorStats Supervisor::stats() const {
    SupervisorStats stats;
    stats.mode = mode_;
    stats.inspected = inspected_;
    stats.terminated = terminated_;
    stats.termination_failed = termination_failed_;
    stats.pid_reuse_skipped = pid_reuse_skipped_;
    stats.retried = retried_;
    stats.inspection_failed = inspection_failed_;
    stats.inspection_degraded = inspection_degraded_;
    stats.events_dropped = events_dropped_;
    stats.reconciliations = reconciliations_;
    stats.unreadable = unreadable_;
    stats.protection_spared = spared_;
    {
        std::scoped_lock lock(policy_mutex_);
        stats.policy_revision = policy_ ? policy_->revision : 0;
    }
    stats.lineage_saturated = lineage_.saturated();
    stats.lineage_records = lineage_.size();
    {
        std::scoped_lock lock(queue_mutex_);
        stats.queue_depth = events_.size() + sweeps_.size();
        stats.queue_high_water = high_water_;
    }
    {
        std::scoped_lock lock(retry_mutex_);
        stats.retries_waiting = retries_.size();
    }
    {
        std::scoped_lock lock(tracked_mutex_);
        stats.tracked = tracked_.size();
    }
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
    if (!assessments_.empty()) {
        std::vector<double> sorted(assessments_.begin(), assessments_.end());
        std::ranges::sort(sorted);
        double total = 0;
        for (const double value : sorted) total += value;
        stats.assessment_samples = sorted.size();
        stats.assessment_average_us = total / static_cast<double>(sorted.size());
        stats.assessment_p95_us = sorted[(sorted.size() - 1) * 95 / 100];
        stats.assessment_max_us = sorted.back();
    }
    return stats;
}

void Supervisor::enqueue(std::uint32_t pid, std::uint64_t event_ticks, bool from_event) {
    bool dropped = false;
    {
        std::scoped_lock lock(queue_mutex_);
        auto& queue = from_event ? events_ : sweeps_;
        if (queue.size() >= kMaxQueue) {
            dropped = true;
        } else {
            queue.push_back({pid, event_ticks, 0});
            ++active_[pid];
            high_water_ = std::max(high_water_, events_.size() + sweeps_.size());
        }
    }
    if (dropped) {
        if (from_event) {
            ++events_dropped_;
            reconcile_requested_ = true;
            std::scoped_lock lock(wake_mutex_);
            wake_.notify_all();
        }
        return;
    }
    queue_ready_.notify_one();
}

void Supervisor::schedule_retry(Pending pending) {
    const auto delay = pending.waiting_for_parent
                           ? kParentWait
                           : kBackoff[std::min<std::size_t>(pending.attempt - 1, kBackoff.size() - 1)];
    {
        std::scoped_lock lock(retry_mutex_);
        retries_.push_back({Clock::now() + delay, pending});
    }
    retries_changed_ = true;
    std::scoped_lock lock(wake_mutex_);
    wake_.notify_all();
}

void Supervisor::promote_due_retries() {
    std::vector<Pending> due;
    {
        std::scoped_lock lock(retry_mutex_);
        const auto now = Clock::now();
        for (auto it = retries_.begin(); it != retries_.end();) {
            if (it->due <= now) {
                due.push_back(it->pending);
                it = retries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (due.empty()) return;
    {
        std::scoped_lock lock(queue_mutex_);
        for (auto& pending : due) events_.push_front(pending);
        high_water_ = std::max(high_water_, events_.size() + sweeps_.size());
    }
    queue_ready_.notify_all();
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
        Pending pending;
        {
            std::unique_lock lock(queue_mutex_);
            if (!queue_ready_.wait(lock, stop, [this] { return !events_.empty() || !sweeps_.empty(); }))
                return;
            auto& queue = events_.empty() ? sweeps_ : events_;
            pending = queue.front();
            queue.pop_front();
        }
        try {
            pending.waiting_for_parent = false;
            pending.wait_parent = 0;
            if (inspect(pending) == Outcome::Retry) {
                ++(pending.waiting_for_parent ? pending.waits : pending.attempt);
                schedule_retry(pending);
            } else {
                finished(pending.pid);
            }
        } catch (const std::exception&) {
            ++inspection_failed_;
            finished(pending.pid);
        }
    }
}

void Supervisor::maintain(const std::stop_token& stop) {
    auto last_restart = Clock::now();
    auto next_periodic = Clock::now() + kEventSweepInterval;
    std::unique_lock lock(wake_mutex_);
    while (!stop.stop_requested()) {
        auto deadline = next_periodic;
        {
            std::scoped_lock retry_lock(retry_mutex_);
            for (const auto& delayed : retries_) deadline = std::min(deadline, delayed.due);
        }
        wake_.wait_until(lock, stop, deadline,
                         [this] { return reconcile_requested_.load() || retries_changed_.load(); });
        retries_changed_ = false;
        if (stop.stop_requested() || mode_ == SupervisionMode::Stopped) return;
        lock.unlock();

        promote_due_retries();
        const auto now = Clock::now();
        const bool periodic = now >= next_periodic;
        bool reconcile = reconcile_requested_.exchange(false);
        if (periodic) {
            if (mode_ == SupervisionMode::Events && !trace_.alive()) {
                set_mode(SupervisionMode::Polling, "The process event session ended unexpectedly.");
                last_restart = now;
            } else if (mode_ == SupervisionMode::Polling && now - last_restart >= kRestartInterval) {
                last_restart = now;
                std::string error;
                if (trace_.start(error)) {
                    set_mode(SupervisionMode::Events, "");
                    reconcile = true;
                }
            }
            forget_exited();
            lineage_.sweep();
            next_periodic =
                now + (mode_ == SupervisionMode::Events ? kEventSweepInterval : kPollingSweepInterval);
        }
        if (periodic || reconcile) {
            if (reconcile) ++reconciliations_;
            sweep();
        }
        lock.lock();
    }
}

bool Supervisor::in_flight(std::uint32_t pid, std::uint32_t except) {
    if (pid == except) return false;
    std::scoped_lock lock(queue_mutex_);
    return active_.contains(pid);
}

bool Supervisor::relevant_to_observation(std::uint32_t pid) {
    const auto process = system::ProcessRef::open(pid);
    const auto path = process ? process->image_path() : std::nullopt;
    if (!path) return false;
    core::AppIdentity identity;
    identity.path = system::path_to_utf8(*path);
    identity.package_family = process->package_family();
    return discovery_.is_root(identity) || discovery_.related(identity);
}

void Supervisor::finished(std::uint32_t pid) {
    {
        std::scoped_lock lock(queue_mutex_);
        const auto it = active_.find(pid);
        if (it != active_.end() && --it->second <= 0) active_.erase(it);
    }
    wake_waiters(pid);
}

void Supervisor::wake_waiters(std::uint32_t pid) {
    bool woke = false;
    {
        std::scoped_lock lock(retry_mutex_);
        const auto now = Clock::now();
        for (auto& delayed : retries_) {
            if (delayed.pending.wait_parent == pid) {
                delayed.due = now;
                woke = true;
            }
        }
    }
    if (woke) {
        retries_changed_ = true;
        std::scoped_lock lock(wake_mutex_);
        wake_.notify_all();
    }
}

void Supervisor::record_assessment(std::chrono::steady_clock::duration elapsed) {
    const double micros = std::chrono::duration<double, std::micro>(elapsed).count();
    std::scoped_lock lock(stats_mutex_);
    assessments_.push_back(micros);
    if (assessments_.size() > kLatencyWindow) assessments_.pop_front();
}

bool Supervisor::claim(std::uint32_t pid, std::uint64_t created_ticks, bool retrying) {
    std::scoped_lock lock(tracked_mutex_);
    auto [it, inserted] = tracked_.try_emplace(pid, Tracked{created_ticks, State::InFlight});
    if (inserted) return true;
    if (it->second.created != created_ticks) {
        it->second = {created_ticks, State::InFlight};
        return true;
    }
    if (retrying && it->second.state == State::Retrying) {
        it->second.state = State::InFlight;
        return true;
    }
    return false;
}

void Supervisor::mark(std::uint32_t pid, std::uint64_t created_ticks, State state) {
    std::scoped_lock lock(tracked_mutex_);
    const auto it = tracked_.find(pid);
    if (it != tracked_.end() && it->second.created == created_ticks) it->second.state = state;
}

void Supervisor::forget_exited() {
    const auto alive = system::running_process_ids();
    const std::unordered_set<std::uint32_t> present(alive.begin(), alive.end());
    std::scoped_lock lock(tracked_mutex_);
    std::erase_if(tracked_, [&](const auto& entry) { return !present.contains(entry.first); });
}

void Supervisor::record_latency(std::uint64_t event_ticks) {
    const auto now = system::ProcessRef::now_ticks();
    if (event_ticks == 0 || now < event_ticks) return;
    std::scoped_lock lock(stats_mutex_);
    latencies_.push_back(static_cast<double>(now - event_ticks) / static_cast<double>(kTicksPerMillisecond));
    if (latencies_.size() > kLatencyWindow) latencies_.pop_front();
}

Supervisor::Outcome Supervisor::retry_or_fail(const Pending& pending, const char* stage, std::uint32_t error,
                                              bool final) {
    if (!final) {
        ++retried_;
        return Outcome::Retry;
    }
    ++inspection_failed_;
    if (on_failure_) on_failure_({pending.pid, stage, error, pending.attempt + 1, false});
    return Outcome::Settled;
}

void Supervisor::observe(const system::ProcessRef& process, Assessment& assessment, std::uint64_t created) {
    const auto& facts = assessment.facts;
    if (!facts.image_known) return;
    const bool root = discovery_.is_root(facts.app);
    const bool descendant = std::ranges::find(facts.lineage, kObservedMember) != facts.lineage.end();
    if (!root && !descendant && !discovery_.related(facts.app)) return;
    if (root || descendant) {
        lineage_.remember(process, created, assessment.sid, {kObservedMember});
        wake_waiters(process.pid());
    }
    assessor_.resolve_fully(assessment, process, false);

    Observation observation;
    observation.identity = assessment.facts.app;
    observation.root = root;
    observation.descendant = descendant;
    observation.windows_protected = assessment.decision.source == core::DecisionSource::Protected;
    if (assessment.parent_pid != 0) {
        if (const auto parent = system::ProcessRef::open(assessment.parent_pid)) {
            if (const auto path = parent->image_path()) observation.parent_path = system::path_to_utf8(*path);
        }
    }
    discovery_.note(observation);
    assessor_.resolve_fully(assessment, process);
    discovery_.complete(assessment.facts.app);
}

Supervisor::Outcome Supervisor::inspect(Pending& pending) {
    std::shared_ptr<const EnforcementPolicy> policy;
    {
        std::scoped_lock lock(policy_mutex_);
        policy = policy_;
    }
    if ((!policy && !discovery_.active()) || pending.pid <= 4 || pending.pid == GetCurrentProcessId())
        return Outcome::Settled;

    const bool final = pending.attempt + 1 >= kMaxAttempts;
    const bool stop_waiting = pending.waits >= kMaxParentWaits;
    std::uint32_t open_error = 0;
    const auto process = system::ProcessRef::open(pending.pid, &open_error);
    if (!process) {
        if (open_error == ERROR_INVALID_PARAMETER) return Outcome::Settled;
        if (open_error == ERROR_ACCESS_DENIED) {
            ++unreadable_;
            return Outcome::Settled;
        }
        return retry_or_fail(pending, "open", open_error, final);
    }
    const auto created = process->creation_ticks();
    if (!created) {
        if (process->exited()) return Outcome::Settled;
        return retry_or_fail(pending, "times", GetLastError(), final);
    }
    if (pending.event_ticks != 0 && *created > pending.event_ticks + kReuseTolerance) {
        ++pid_reuse_skipped_;
        return Outcome::Settled;
    }
    if (!claim(pending.pid, *created, pending.attempt > 0 || pending.waits > 0)) return Outcome::Settled;
    if (pending.attempt == 0 && pending.waits == 0) ++inspected_;

    const auto settle = [&](Outcome outcome) {
        mark(pending.pid, *created, outcome == Outcome::Retry ? State::Retrying : State::Settled);
        return outcome;
    };
    const auto retry = [&](const char* stage, std::uint32_t error) {
        return settle(retry_or_fail(pending, stage, error, final));
    };

    const auto sid = process->user_sid();
    if (!sid) {
        if (process->exited()) return settle(Outcome::Settled);
        return retry("token", GetLastError());
    }
    const bool guest = policy && policy->guest_sids.contains(*sid) && !is_service_account(*sid);
    const bool observed = discovery_.wants(*sid);
    if (!guest && !observed) return settle(Outcome::Settled);

    const auto started = Clock::now();
    auto assessment = assessor_.assess(
        *process, *created, *sid, guest ? policy.get() : nullptr, final,
        stop_waiting ? Assessor::ParentPending{} : [&](std::uint32_t parent) {
            return in_flight(parent, pending.pid) && (guest || relevant_to_observation(parent));
        });
    record_assessment(Clock::now() - started);
    if (assessment.retry_stage) {
        if (process->exited()) return settle(Outcome::Settled);
        if (std::string_view(assessment.retry_stage) == "parent") {
            pending.waiting_for_parent = true;
            pending.wait_parent = assessment.parent_pid;
            ++retried_;
            return settle(Outcome::Retry);
        }
        return retry(assessment.retry_stage, assessment.error);
    }
    if (assessment.degraded && on_failure_) {
        on_failure_({pending.pid, assessment.degraded_stage, assessment.error, pending.attempt + 1, true});
    }
    if (observed) observe(*process, assessment, *created);
    if (!guest) return settle(Outcome::Settled);

    if (assessment.degraded) ++inspection_degraded_;
    if (!assessment.image.empty() && !final) {
        const auto again = process->image_path();
        if (!again || *again != assessment.image) return retry("unstable_image", 0);
    }
    const auto& decision = assessment.decision;
    Denial denial;
    denial.pid = pending.pid;
    denial.session_id = assessment.session;
    denial.user_sid = *sid;
    denial.app = assessment.facts.app;
    denial.decision = decision;
    denial.profile_id = policy->profile_id;
    denial.profile_name = policy->profile_name;
    denial.audit_only = policy->audit_only;
    if (assessment.parent_pid != 0) {
        if (const auto parent = system::ProcessRef::open(assessment.parent_pid)) {
            if (const auto path = parent->image_path()) denial.parent_path = system::path_to_utf8(*path);
        }
    }

    if (decision.action == core::Action::Allow) {
        const bool by_evidence = decision.source == core::DecisionSource::Protected &&
                                 decision.reason == core::protected_kind_name(core::ProtectedKind::ComServer);
        if (on_allow_ && (decision.source != core::DecisionSource::Protected || by_evidence))
            on_allow_(denial);
        return settle(Outcome::Settled);
    }

    if (!policy->audit_only) {
        switch (process->terminate_checked()) {
            case system::ProcessRef::Termination::Done:
                denial.enforced = true;
                ++terminated_;
                record_latency(pending.event_ticks);
                break;
            case system::ProcessRef::Termination::Refused:
                denial.spared = true;
                ++spared_;
                break;
            case system::ProcessRef::Termination::Failed:
                if (process->exited()) return settle(Outcome::Settled);
                if (!final) return retry("terminate", GetLastError());
                ++termination_failed_;
                break;
        }
    }
    on_denial_(denial);
    return settle(Outcome::Settled);
}

}  // namespace houseguest::service
