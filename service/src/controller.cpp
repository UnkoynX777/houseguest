#include "controller.hpp"

#include <windows.h>

#include <algorithm>

#include "houseguest/core/analysis.hpp"
#include "houseguest/system/accounts.hpp"
#include "houseguest/system/capabilities.hpp"
#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"

namespace houseguest::service {
namespace {

using core::Json;
using std::chrono::seconds;

constexpr std::uint32_t kPinIterations = 600000;
constexpr std::size_t kMinPin = 4;
constexpr std::size_t kMaxPin = 64;
constexpr auto kTokenLifetime = std::chrono::minutes(15);
constexpr auto kNoticeInterval = std::chrono::seconds(10);

const char* severity_name(core::Severity severity) {
    switch (severity) {
        case core::Severity::Info:
            return "info";
        case core::Severity::Warning:
            return "warning";
        case core::Severity::Error:
            return "error";
    }
    return "info";
}

const char* code_name(core::FindingCode code) {
    switch (code) {
        case core::FindingCode::InvalidMatcher:
            return "invalid_matcher";
        case core::FindingCode::DuplicateRuleId:
            return "duplicate_rule_id";
        case core::FindingCode::ConflictingRules:
            return "conflicting_rules";
        case core::FindingCode::ShadowedRule:
            return "shadowed_rule";
        case core::FindingCode::ExpiredRule:
            return "expired_rule";
        case core::FindingCode::BaselineProtected:
            return "baseline_protected";
        case core::FindingCode::NoAllowances:
            return "no_allowances";
    }
    return "unknown";
}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

const char* enforcement_name(SupervisionMode mode) {
    switch (mode) {
        case SupervisionMode::Events:
            return "supervised";
        case SupervisionMode::Polling:
            return "polling";
        case SupervisionMode::Stopped:
            break;
    }
    return "unavailable";
}

}  // namespace

Controller::Controller(std::filesystem::path data_directory, bool enable_enforcement)
    : store_(std::move(data_directory)), install_dir_(executable_directory()) {
    live_ = enable_enforcement;
    load_result_ = store_.load();
    audit_ = std::make_unique<AuditLog>(store_.directory() / L"audit.log",
                                        store_.derive_key("houseguest.audit.v2"));
    switch (load_result_) {
        case LoadResult::Tampered:
            audit_->append("service", "state.integrity_failure", {{"action", "started unconfigured"}});
            break;
        case LoadResult::Recovered:
            audit_->append("service", "state.recovered", {{"source", "previous snapshot"}});
            break;
        default:
            break;
    }

    capabilities_ = system::probe_capabilities();
    supervisor_ = std::make_unique<Supervisor>(
        resolver_, [this](const Denial& d) { on_denial(d); },
        [this](SupervisionMode mode, const std::string& reason) { on_mode(mode, reason); },
        [this](const InspectionFailure& failure) { on_failure(failure); });
    if (enable_enforcement) supervisor_->start();
    register_commands();

    {
        std::scoped_lock lock(mutex_);
        if (store_.state.session) {
            auto& session = *store_.state.session;
            if (session.ending()) {
                try {
                    arm(session);
                } catch (const std::exception&) {
                }
                finish_session();
            } else if (session.ends_at <= now_seconds()) {
                end_session("expired while service was stopped", true);
            } else {
                try {
                    arm(session);
                    audit_->append("service", "session.resumed", {{"profile", session.profile_id}});
                } catch (const std::exception& e) {
                    audit_->append("service", "session.resume_failed", {{"reason", e.what()}});
                    if (session.on_expiry == "none") session.on_expiry = "disconnect";
                    end_session("protection could not be restored after a restart", true);
                }
            }
        }
        reconcile_managed_accounts();
    }
    timer_ = std::jthread([this](const std::stop_token& stop) { timer_loop(stop); });
}

Controller::~Controller() {
    shutdown();
}

void Controller::shutdown() {
    if (timer_.joinable()) {
        timer_.request_stop();
        timer_wake_.notify_all();
        timer_.join();
    }
    if (supervisor_) supervisor_->stop();
}

Controller::Compiled Controller::compile(const core::Profile& source,
                                         const std::vector<core::Rule>& exceptions,
                                         const std::vector<std::string>& guests, bool audit_only) const {
    Compiled result;
    result.policy = std::make_shared<EnforcementPolicy>();
    auto& policy = *result.policy;
    policy.profile = source;
    policy.profile.rules.insert(policy.profile.rules.end(), exceptions.begin(), exceptions.end());
    for (auto& rule : policy.profile.rules) {
        if (auto* path = std::get_if<core::PathMatcher>(&rule.matcher))
            path->pattern = system::expand_known_folders(path->pattern);
        policy.needs_hash |= std::holds_alternative<core::HashMatcher>(rule.matcher);
        policy.needs_publisher |= std::holds_alternative<core::PublisherMatcher>(rule.matcher);
        if (std::holds_alternative<core::HashMatcher>(rule.matcher) ||
            std::holds_alternative<core::PublisherMatcher>(rule.matcher)) {
            policy.highest_expensive_priority = std::max(policy.highest_expensive_priority, rule.priority);
        }
    }
    policy.baseline = core::Baseline::windows_default(system::expand_known_folders("%SystemRoot%"),
                                                      system::path_to_utf8(install_dir_));
    policy.guest_sids.insert(guests.begin(), guests.end());
    policy.audit_only = audit_only;

    const auto findings = core::analyze(policy.profile, policy.baseline, std::chrono::system_clock::now());
    for (const auto& finding : findings) {
        result.has_errors |= finding.severity == core::Severity::Error;
        result.has_warnings |= finding.severity == core::Severity::Warning;
        result.findings.push_back({{"severity", severity_name(finding.severity)},
                                   {"code", code_name(finding.code)},
                                   {"rules", finding.rule_ids},
                                   {"message", finding.message}});
    }
    if (std::ranges::any_of(policy.profile.rules, [](const core::Rule& rule) {
            return std::holds_alternative<core::PackageMatcher>(rule.matcher);
        })) {
        result.findings.push_back(
            {{"severity", "info"},
             {"code", "package_not_previewable"},
             {"rules", Json::array()},
             {"message",
              "Package family rules are evaluated on running processes. The preview lists "
              "installed apps by path and cannot show their package decisions."}});
    }
    if (std::ranges::find(guests, store_.state.owner_sid) != guests.end()) {
        result.has_warnings = true;
        result.findings.push_back({{"severity", "warning"},
                                   {"code", "owner_account_restricted"},
                                   {"rules", Json::array()},
                                   {"message",
                                    "The owner's own account is restricted. Houseguest itself stays "
                                    "available so the session can be ended with the PIN."}});
    }
    return result;
}

void Controller::arm(const Session& session) {
    const auto& profile = find_profile(session.profile_id);
    auto compiled = compile(profile, session.exceptions, session.guest_sids, session.audit_only);
    if (compiled.has_errors)
        throw Failure("policy_invalid", "Policy has errors.", {{"findings", compiled.findings}});
    supervisor_->apply(compiled.policy);
    if (supervisor_->running()) supervisor_->sweep();
}

void Controller::end_session(const std::string& reason, bool apply_expiry_action) {
    if (!store_.state.session) return;
    auto& session = *store_.state.session;
    if (!session.ending()) {
        session.phase = "ending";
        session.end_reason = reason;
        session.end_apply_action = apply_expiry_action;
        store_.save();
        audit_->append("service", "session.ending", {{"reason", reason}, {"profile", session.profile_id}});
        end_attempts_ = 0;
    }
    finish_session();
}

bool Controller::finish_session() {
    if (!store_.state.session) return true;
    const auto session = *store_.state.session;
    bool complete = true;

    std::vector<std::string> managed_guests;
    for (const auto& sid : session.guest_sids) {
        if (is_managed(sid)) managed_guests.push_back(sid);
    }
    if (session.end_apply_action && session.on_expiry != "none") {
        const bool logoff = session.on_expiry == "logoff" || session.on_expiry == "disable";
        complete &= end_user_sessions(session.guest_sids, logoff);
    }
    for (const auto& sid : managed_guests) complete &= set_account_disabled(sid, true);
    if (!managed_guests.empty()) complete &= end_user_sessions(managed_guests, true);

    if (!complete) {
        if (end_attempts_++ % 6 == 0) {
            audit_->append("service", "session.end_incomplete",
                           {{"reason", session.end_reason}, {"attempt", end_attempts_}});
        }
        return false;
    }
    supervisor_->apply(nullptr);
    store_.state.session.reset();
    store_.save();
    audit_->append("service", "session.ended",
                   {{"reason", session.end_reason}, {"profile", session.profile_id}});
    return true;
}

void Controller::force_end_session(const std::string& reason) {
    if (!store_.state.session) return;
    const auto session = *store_.state.session;
    supervisor_->apply(nullptr);
    store_.state.session.reset();
    store_.save();
    audit_->append("service", "session.forced_end", {{"reason", reason}, {"profile", session.profile_id}});
}

bool Controller::set_account_disabled(const std::string& sid, bool disabled) const {
    return !live_ || system::set_user_disabled(sid, disabled);
}

bool Controller::set_account_expiry(const std::string& sid, std::int64_t unix_seconds) const {
    return !live_ || system::set_user_expiry(sid, unix_seconds);
}

bool Controller::end_user_sessions(const std::vector<std::string>& sids, bool logoff) const {
    return !live_ || system::disconnect_sessions(sids, logoff).complete;
}

bool Controller::is_managed(const std::string& sid) const {
    return std::ranges::any_of(store_.state.managed,
                               [&](const ManagedAccount& account) { return account.sid == sid; });
}

void Controller::reconcile_managed_accounts() {
    std::vector<std::string> idle;
    for (const auto& account : store_.state.managed) {
        const bool in_session =
            store_.state.session && std::ranges::find(store_.state.session->guest_sids, account.sid) !=
                                        store_.state.session->guest_sids.end();
        if (in_session) continue;
        if (!live_ || !system::user_exists(account.sid)) continue;
        if (!set_account_disabled(account.sid, true)) {
            audit_->append("service", "managed_account.disable_failed", {{"name", account.name}});
            continue;
        }
        idle.push_back(account.sid);
    }
    if (!idle.empty()) end_user_sessions(idle, true);
}

void Controller::timer_loop(const std::stop_token& stop) {
    std::unique_lock lock(mutex_);
    while (!stop.stop_requested()) {
        if (!store_.state.session) {
            timer_wake_.wait(lock, stop, [this] { return store_.state.session.has_value(); });
            continue;
        }
        if (store_.state.session->ending()) {
            timer_wake_.wait_for(lock, stop, std::chrono::seconds(10), [] { return false; });
            if (!stop.stop_requested()) finish_session();
            continue;
        }
        const auto ends_at = store_.state.session->ends_at;
        const auto deadline = std::chrono::system_clock::time_point{seconds{ends_at}};
        const auto now = std::chrono::system_clock::now();
        if (now >= deadline) {
            end_session("expired", true);
            continue;
        }
        timer_wake_.wait_until(
            lock, stop, std::min(deadline, now + std::chrono::minutes(1)),
            [this, ends_at] { return !store_.state.session || store_.state.session->ends_at != ends_at; });
    }
}

void Controller::on_denial(const Denial& denial) {
    ++blocked_;
    const auto key = denial.user_sid + "|" + denial.app.path;
    const auto now = std::chrono::steady_clock::now();
    {
        std::scoped_lock lock(notice_mutex_);
        const auto it = last_notice_.find(key);
        if (it != last_notice_.end() && now - it->second < kNoticeInterval) return;
        if (last_notice_.size() > 512) last_notice_.clear();
        last_notice_[key] = now;
    }
    audit_->append(
        "supervisor",
        denial.enforced     ? "app.blocked"
        : denial.audit_only ? "app.would_block"
                            : "app.block_failed",
        {{"path", denial.app.path},
         {"sha256", denial.app.sha256},
         {"publisher", denial.app.publisher},
         {"user_sid", denial.user_sid},
         {"rule", denial.decision.rule_id},
         {"source", denial.decision.source == core::DecisionSource::Default ? "default" : "rule"}});
    if (denial.enforced && denial.session_id != 0) {
        const auto name = system::path_from_utf8(denial.app.path).filename().wstring();
        system::notify_session(denial.session_id, L"Houseguest",
                               name + L" is not allowed on this computer right now.");
    }
}

void Controller::on_mode(SupervisionMode mode, const std::string& reason) {
    audit_->append("service", "supervisor.mode", {{"mode", enforcement_name(mode)}, {"reason", reason}});
}

void Controller::on_failure(const InspectionFailure& failure) {
    const auto key = failure.stage + (failure.degraded ? "/degraded" : "");
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t suppressed = 0;
    {
        std::scoped_lock lock(failure_mutex_);
        auto& entry = failure_notice_[key];
        if (entry.first != std::chrono::steady_clock::time_point{} &&
            now - entry.first < std::chrono::seconds(10)) {
            ++entry.second;
            return;
        }
        suppressed = entry.second;
        entry = {now, 0};
    }
    audit_->append("supervisor", "inspection.failed",
                   {{"stage", failure.stage},
                    {"pid", failure.pid},
                    {"error", failure.error},
                    {"attempts", failure.attempts},
                    {"degraded", failure.degraded},
                    {"suppressed", suppressed}});
}

void Controller::audit_limited(const std::string& key, const std::string& actor, const std::string& event,
                               Json detail) {
    const auto now = std::chrono::steady_clock::now();
    std::uint64_t suppressed = 0;
    {
        std::scoped_lock lock(limited_mutex_);
        if (limited_notice_.size() > 256) limited_notice_.clear();
        auto& entry = limited_notice_[key];
        if (entry.first != std::chrono::steady_clock::time_point{} && now - entry.first < kNoticeInterval) {
            ++entry.second;
            return;
        }
        suppressed = entry.second;
        entry = {now, 0};
    }
    if (suppressed != 0) detail["suppressed"] = suppressed;
    audit_->append(actor, event, std::move(detail));
}

void Controller::set_ipc_stats(std::function<Json()> provider) {
    std::scoped_lock lock(mutex_);
    ipc_stats_ = std::move(provider);
}

Json Controller::supervisor_json() const {
    const auto stats = supervisor_->stats();
    return {{"mode", enforcement_name(stats.mode)},
            {"degraded_reason", stats.degraded_reason},
            {"inspected", stats.inspected},
            {"terminated", stats.terminated},
            {"termination_failed", stats.termination_failed},
            {"pid_reuse_skipped", stats.pid_reuse_skipped},
            {"retried", stats.retried},
            {"inspection_failed", stats.inspection_failed},
            {"inspection_degraded", stats.inspection_degraded},
            {"unreadable", stats.unreadable},
            {"events_dropped", stats.events_dropped},
            {"reconciliations", stats.reconciliations},
            {"queue_depth", stats.queue_depth},
            {"queue_high_water", stats.queue_high_water},
            {"retries_waiting", stats.retries_waiting},
            {"tracked_processes", stats.tracked},
            {"identity_cache_entries", resolver_.cached()},
            {"latency_ms",
             {{"samples", stats.latency_samples},
              {"average", stats.latency_average_ms},
              {"p95", stats.latency_p95_ms},
              {"max", stats.latency_max_ms}}}};
}

core::Profile& Controller::find_profile(const std::string& id) {
    const auto it = std::ranges::find(store_.state.profiles, id, &core::Profile::id);
    if (it == store_.state.profiles.end()) throw Failure("not_found", "Profile not found.");
    return *it;
}

bool Controller::session_uses(const std::string& profile_id) const {
    return store_.state.session && store_.state.session->profile_id == profile_id;
}

std::string Controller::issue_token(const std::string& sid) {
    const auto token = system::to_hex(system::random_bytes(32));
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock(token_mutex_);
    std::erase_if(tokens_, [&](const auto& item) { return item.second.expires < now; });
    tokens_[token] = {sid, now + kTokenLifetime};
    return token;
}

void Controller::require_owner(const Caller& caller, const Json& request) {
    const auto it = request.find("token");
    if (it == request.end() || !it->is_string())
        throw Failure("unauthorized", "Unlock with the owner PIN first.");
    std::scoped_lock lock(token_mutex_);
    const auto found = tokens_.find(it->get<std::string>());
    const auto now = std::chrono::steady_clock::now();
    if (found == tokens_.end() || found->second.expires < now || found->second.sid != caller.sid ||
        caller.sid != store_.state.owner_sid) {
        throw Failure("unauthorized", "Unlock with the owner PIN first.");
    }
    found->second.expires = now + kTokenLifetime;
}

void Controller::set_pin(const std::string& pin) {
    if (pin.size() < kMinPin || pin.size() > kMaxPin)
        throw Failure("bad_request", "PIN must have 4 to 64 characters.");
    const auto salt = system::random_bytes(16);
    const auto hash = system::pbkdf2_sha256(pin, salt, kPinIterations, 32);
    store_.state.pin = {system::to_hex(salt), system::to_hex(hash), kPinIterations};
}

void Controller::verify_pin(const std::string& pin) {
    auto& state = store_.state;
    const auto now = now_seconds();
    if (state.locked_until > now) {
        throw Failure("rate_limited", "Too many attempts.", {{"retry_after", state.locked_until - now}});
    }
    const auto salt = system::from_hex(state.pin.salt_hex);
    const auto expected = system::from_hex(state.pin.hash_hex);
    bool match = false;
    if (salt && expected && pin.size() <= kMaxPin) {
        const auto actual = system::pbkdf2_sha256(pin, *salt, state.pin.iterations, expected->size());
        match = system::constant_time_equal(actual, *expected);
    }
    if (match) {
        if (state.failed_attempts != 0) {
            state.failed_attempts = 0;
            state.locked_until = 0;
            store_.save();
        }
        return;
    }
    ++state.failed_attempts;
    if (state.failed_attempts >= 3) {
        const int shift = std::min(state.failed_attempts - 3, 5);
        state.locked_until = now + std::min<std::int64_t>(900, 30LL << shift);
    }
    store_.save();
    audit_->append("auth", "pin.failed", {{"attempts", state.failed_attempts}});
    throw Failure("invalid_pin", "Incorrect PIN.",
                  {{"attempts", state.failed_attempts},
                   {"retry_after", std::max<std::int64_t>(0, state.locked_until - now)}});
}

Json Controller::status_json() const {
    const auto& state = store_.state;
    Json status{{"version", kVersion},
                {"configured", state.configured()},
                {"armed", state.session.has_value()},
                {"now", now_seconds()},
                {"enforcement", enforcement_name(supervisor_->mode())},
                {"store_tampered", load_result_ == LoadResult::Tampered},
                {"language", state.language}};
    if (state.session) {
        status["ends_at"] = state.session->ends_at;
        status["ending"] = state.session->ending();
        const auto profile = std::ranges::find(state.profiles, state.session->profile_id, &core::Profile::id);
        if (profile != state.profiles.end()) status["profile"] = profile->name;
    }
    return status;
}

Json Controller::handle(const Caller& caller, const Json& request) {
    const auto id = request.contains("id") ? request["id"] : Json(nullptr);
    try {
        if (!request.is_object()) throw Failure("bad_request", "Request must be an object.");
        const auto command = require_text(request, "cmd");
        const auto found = commands_.find(command);
        if (found == commands_.end()) throw Failure("bad_request", "Unknown command.");
        const auto& entry = found->second;
        const Json args =
            request.contains("args") && request["args"].is_object() ? request["args"] : Json::object();

        Json data;
        auto execute = [&] {
            if (entry.access == Access::Elevated && !caller.elevated) {
                throw Failure("forbidden", "Administrator elevation is required.");
            }
            if (entry.access == Access::Owner) require_owner(caller, request);
            return entry.run(caller, args);
        };
        if (entry.unlocked) {
            data = execute();
        } else {
            std::scoped_lock lock(mutex_);
            data = execute();
        }
        return {{"id", id}, {"ok", true}, {"data", std::move(data)}};
    } catch (const Failure& failure) {
        return {
            {"id", id},
            {"ok", false},
            {"error", {{"code", failure.code}, {"message", failure.what()}, {"details", failure.details}}}};
    } catch (const Json::exception& e) {
        return {{"id", id},
                {"ok", false},
                {"error", {{"code", "bad_request"}, {"message", e.what()}, {"details", Json::object()}}}};
    } catch (const std::invalid_argument& e) {
        return {{"id", id},
                {"ok", false},
                {"error", {{"code", "bad_request"}, {"message", e.what()}, {"details", Json::object()}}}};
    } catch (const std::exception& e) {
        audit_limited("internal_error", "service", "internal_error", {{"message", e.what()}});
        return {
            {"id", id},
            {"ok", false},
            {"error", {{"code", "internal"}, {"message", "Internal error."}, {"details", Json::object()}}}};
    }
}

void Controller::add(const char* name, Access access, std::function<Json(const Caller&, const Json&)> run,
                     bool unlocked) {
    commands_.emplace(name, Command{access, unlocked, std::move(run)});
}

void Controller::register_commands() {
    register_account_commands();
    register_policy_commands();
    register_session_commands();
    register_system_commands();
}

}  // namespace houseguest::service
