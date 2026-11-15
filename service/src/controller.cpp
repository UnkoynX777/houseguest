#include "controller.hpp"

#include <windows.h>

#include <algorithm>

#include "houseguest/core/analysis.hpp"
#include "houseguest/core/glob.hpp"
#include "houseguest/core/protection.hpp"
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
        case core::FindingCode::InvalidComponent:
            return "invalid_component";
        case core::FindingCode::DuplicateComponentId:
            return "duplicate_component_id";
        case core::FindingCode::ApplicationWithoutMain:
            return "application_without_main";
        case core::FindingCode::WeakIndependent:
            return "weak_independent";
        case core::FindingCode::UnpinnedComponent:
            return "unpinned_component";
        case core::FindingCode::SensitiveComponent:
            return "sensitive_component";
        case core::FindingCode::ApplicationBlockedByRule:
            return "application_blocked_by_rule";
        case core::FindingCode::RelatedWithoutSigner:
            return "related_without_signer";
        case core::FindingCode::ExpiredComponent:
            return "expired_component";
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
    : store_(std::move(data_directory)),
      catalog_(store_.directory() / L"catalog.json"),
      install_dir_(executable_directory()),
      continuity_(install_dir_),
      assessor_(resolver_, continuity_, lineage_, system::expand_known_folders("%SystemRoot%"),
                system::expand_known_folders("%ProgramFiles%")) {
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
    continuity_.pin();
    supervisor_ = std::make_unique<Supervisor>(
        assessor_, lineage_, discovery_, [this](const Denial& d) { on_denial(d); },
        [this](SupervisionMode mode, const std::string& reason) { on_mode(mode, reason); },
        [this](const InspectionFailure& failure) { on_failure(failure); });
    supervisor_->on_allowed([this](const Denial& allowed) { on_allowed(allowed); });
    if (enable_enforcement) supervisor_->start();
    register_commands();
    if (enable_enforcement) {
        catalog_.refresh(catalog_request());
        watcher_ = std::make_unique<ChangeWatcher>(
            [this] {
                std::scoped_lock lock(mutex_);
                std::vector<std::filesystem::path> roots;
                for (const auto& root : store_.state.portable_roots) roots.push_back(system::path_from_utf8(root));
                return roots;
            },
            [this](const std::string&) { catalog_.refresh(catalog_request()); });
        watcher_->start();
    }

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
    if (watcher_) watcher_->stop();
    if (timer_.joinable()) {
        timer_.request_stop();
        timer_wake_.notify_all();
        timer_.join();
    }
    if (supervisor_) supervisor_->stop();
}

Controller::Compiled Controller::compile(const core::Profile& source,
                                         const std::vector<core::Rule>& exceptions,
                                         const std::vector<std::string>& guests, bool audit_only) {
    Compiled result;
    result.policy = std::make_shared<EnforcementPolicy>();
    auto& policy = *result.policy;
    policy.profile = source;
    policy.profile.rules.insert(policy.profile.rules.end(), exceptions.begin(), exceptions.end());
    for (auto& rule : policy.profile.rules) {
        if (auto* path = std::get_if<core::PathMatcher>(&rule.matcher))
            path->pattern = system::expand_known_folders(path->pattern);
    }
    for (auto& app : policy.profile.apps) {
        for (auto& component : app.components) component.path = system::expand_known_folders(component.path);
    }
    policy.protection = core::Protection::windows_default(system::expand_known_folders("%SystemRoot%"),
                                                          system::expand_known_folders("%ProgramFiles%"));
    policy.guest_sids.insert(guests.begin(), guests.end());
    policy.audit_only = audit_only;
    policy.revision = store_.revision();
    policy.profile_id = source.id;
    policy.profile_name = source.name;

    const auto add = [&](const char* severity, const char* code, std::string message,
                         Json rules = Json::array()) {
        result.has_errors |= std::string_view(severity) == "error";
        result.has_warnings |= std::string_view(severity) == "warning";
        result.findings.push_back({{"severity", severity},
                                   {"code", code},
                                   {"rules", std::move(rules)},
                                   {"message", std::move(message)}});
    };

    const auto findings = core::analyze(policy.profile, policy.protection, std::chrono::system_clock::now());
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
        add("info", "package_not_previewable",
            "Package family rules are evaluated on running processes. The preview lists installed apps by "
            "path "
            "and cannot show their package decisions.");
    }

    const bool owner_restricted = std::ranges::find(guests, store_.state.owner_sid) != guests.end();
    if (owner_restricted) {
        add("warning", "owner_account_restricted",
            "The owner's own account is restricted. Houseguest itself stays available so the session can be "
            "ended with the PIN.");
    }
    const char* critical = owner_restricted && !audit_only ? "error" : owner_restricted ? "warning" : "info";
    const auto continuity = continuity_.report(guests);
    if (!continuity.install_trusted) {
        add(critical, "continuity_untrusted_install",
            "The Houseguest folder can be modified by a restricted account, so Houseguest cannot recognize "
            "its "
            "own files safely. Install it under Program Files.");
    }
    if (!continuity.missing.empty()) {
        add(critical, "continuity_missing_binary",
            "A Houseguest program was not found next to the service, so it cannot be recognized: " +
                continuity.missing.front());
    }
    if (continuity.webview_runtimes.empty()) {
        add(owner_restricted ? "warning" : "info", "continuity_webview_missing",
            "The WebView2 runtime was not found in Program Files. Without it the window cannot open; use "
            "houseguest-cli from an elevated prompt to administer.");
    } else if (!continuity.webview_trusted) {
        add(owner_restricted ? "warning" : "info", "continuity_webview_untrusted",
            "A WebView2 runtime folder can be modified by a restricted account, so its processes are not "
            "treated as part of Houseguest.");
    }
    const auto now = std::chrono::system_clock::now();
    for (const auto& binary : continuity.verified) {
        core::ProcessFacts facts;
        facts.app = {binary.path, binary.sha256, {}, {}};
        facts.hash_resolved = facts.publisher_resolved = true;
        facts.continuity = core::Continuity::Houseguest;
        if (core::evaluate(policy.profile, policy.protection, facts, now).action != core::Action::Allow) {
            add(critical, "continuity_blocked",
                "The policy would block a Houseguest program: " + binary.path);
        }
    }
    return result;
}

void Controller::arm(const Session& session) {
    const auto& profile = find_profile(session.profile_id);
    auto compiled = compile(profile, session.exceptions, session.guest_sids, session.audit_only);
    if (compiled.has_errors)
        throw Failure("policy_invalid", "Policy has errors.", {{"findings", compiled.findings}});
    continuity_.restrict_to(session.guest_sids);
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

void Controller::remember_review(const Denial& denial) {
    const auto& decision = denial.decision;
    const bool changed = decision.note.starts_with("component_changed:");
    if (decision.review_app.empty() && !changed) return;
    Review review;
    review.app_id = decision.review_app;
    review.kind = changed ? "component_changed" : "unknown_dependency";
    review.path = denial.app.path;
    review.sha256 = denial.app.sha256;
    review.publisher = denial.app.publisher;
    review.package_family = denial.app.package_family;
    review.parent_path = denial.parent_path;
    review.profile_id = denial.profile_id;
    review.note = decision.note;
    review.first = review.last = now_seconds();
    review.count = 1;
    const auto key = review.app_id + "|" + core::normalize_path(review.path) + "|" + review.sha256;
    review.id =
        system::to_hex(system::sha256({reinterpret_cast<const std::uint8_t*>(key.data()), key.size()}))
            .substr(0, 16);

    std::scoped_lock lock(review_mutex_);
    if (const auto it = reviews_.find(review.id); it != reviews_.end()) {
        ++it->second.count;
        it->second.last = review.last;
        return;
    }
    if (reviews_.size() >= 200) {
        const auto oldest =
            std::ranges::min_element(reviews_, {}, [](const auto& entry) { return entry.second.last; });
        reviews_.erase(oldest);
    }
    reviews_.emplace(review.id, std::move(review));
}

namespace {

bool unclassified_windows_component(const Denial& denial) {
    static const std::string root = [] {
        wchar_t buffer[MAX_PATH] = {};
        GetWindowsDirectoryW(buffer, MAX_PATH);
        return core::normalize_path(system::path_to_utf8(buffer)) + "/";
    }();
    return core::is_windows_signer(denial.app.publisher) &&
           core::normalize_path(denial.app.path).starts_with(root) &&
           denial.decision.source != core::DecisionSource::Rule;
}

}  // namespace

void Controller::on_denial(const Denial& denial) {
    const auto& decision = denial.decision;
    const auto file = system::path_from_utf8(denial.app.path).filename();
    if (denial.spared) {
        audit_limited("spared|" + denial.app.path, "supervisor", "protection.spared",
                      {{"path", denial.app.path},
                       {"process", system::path_to_utf8(file)},
                       {"reason", "The kernel marks this process as essential, so it was not ended."}});
        return;
    }
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
    remember_review(denial);

    const char* recommendation = !decision.review_app.empty()                       ? "approve_dependency"
                                 : decision.note.starts_with("component_changed:")  ? "reauthorize_component"
                                 : decision.source == core::DecisionSource::Rule    ? "review_rule"
                                 : decision.source == core::DecisionSource::Default ? "allow_if_needed"
                                                                                    : "none";
    audit_->append("supervisor",
                   denial.enforced     ? "app.blocked"
                   : denial.audit_only ? "app.would_block"
                                       : "app.block_failed",
                   {{"path", denial.app.path},
                    {"process", system::path_to_utf8(file)},
                    {"sha256", denial.app.sha256},
                    {"publisher", denial.app.publisher},
                    {"package_family", denial.app.package_family},
                    {"user_sid", denial.user_sid},
                    {"profile_id", denial.profile_id},
                    {"profile", denial.profile_name},
                    {"rule", decision.rule_id},
                    {"source", core::source_name(decision.source)},
                    {"reason", decision.reason},
                    {"note", decision.note},
                    {"related_app", decision.review_app},
                    {"parent", denial.parent_path},
                    {"recommendation", recommendation},
                    {"windows_component", unclassified_windows_component(denial)}});

    if (!denial.enforced || denial.session_id == 0) return;
    const auto notice_key = decision.review_app.empty() ? denial.app.path : "app|" + decision.review_app;
    {
        std::scoped_lock lock(notice_mutex_);
        const auto it = notified_.find(notice_key);
        if (it != notified_.end() && now - it->second < std::chrono::minutes(5)) return;
        if (notified_.size() > 256) notified_.clear();
        notified_[notice_key] = now;
    }
    const auto name = file.wstring();
    system::notify_session(denial.session_id, L"Houseguest",
                           decision.review_app.empty()
                               ? name + L" is not allowed on this computer right now."
                               : name +
                                     L" was needed by an allowed program but is not approved. Ask the "
                                     L"owner to review it in Houseguest.");
}

void Controller::on_allowed(const Denial& allowed) {
    const auto& decision = allowed.decision;
    const auto key = std::to_string(allowed.session_id) + "|" + allowed.user_sid + "|" + allowed.app.path;
    {
        std::scoped_lock lock(allowed_mutex_);
        if (allowed_seen_.size() >= 2048) allowed_seen_.clear();
        if (!allowed_seen_.insert(key).second) return;
    }
    audit_->append("supervisor", "app.allowed",
                   {{"path", allowed.app.path},
                    {"process", system::path_to_utf8(system::path_from_utf8(allowed.app.path).filename())},
                    {"publisher", allowed.app.publisher},
                    {"user_sid", allowed.user_sid},
                    {"profile_id", allowed.profile_id},
                    {"profile", allowed.profile_name},
                    {"rule", decision.rule_id},
                    {"source", core::source_name(decision.source)},
                    {"reason", decision.reason},
                    {"related_app", decision.app_id},
                    {"parent", allowed.parent_path}});
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
            {"protection_spared", stats.protection_spared},
            {"policy_revision", stats.policy_revision},
            {"lineage_records", stats.lineage_records},
            {"lineage_saturated", stats.lineage_saturated},
            {"assessment_us",
             {{"samples", stats.assessment_samples},
              {"average", stats.assessment_average_us},
              {"p95", stats.assessment_p95_us},
              {"max", stats.assessment_max_us}}},
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
                {"language", state.language},
                {"generations",
                 {{"policy", store_.revision()}, {"catalog", catalog_.generation()}, {"history", audit_->sequence()}}}};
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
        const auto started = std::chrono::steady_clock::now();
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
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            std::scoped_lock lock(stats_mutex_);
            auto& stat = command_stats_[command];
            ++stat.count;
            stat.run_us += static_cast<std::uint64_t>(elapsed);
            stat.run_max_us = std::max(stat.run_max_us, static_cast<std::uint64_t>(elapsed));
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

void Controller::note_reply(const Json& request, std::size_t bytes, std::chrono::steady_clock::duration encoding) {
    if (!request.is_object() || !request.contains("cmd") || !request["cmd"].is_string()) return;
    std::scoped_lock lock(stats_mutex_);
    const auto found = command_stats_.find(request["cmd"].get<std::string>());
    if (found == command_stats_.end()) return;
    auto& stat = found->second;
    stat.encode_us += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(encoding).count());
    stat.bytes += bytes;
    stat.bytes_max = std::max<std::uint64_t>(stat.bytes_max, bytes);
}

Json Controller::caches_json() {
    const auto hits = library_hits_.load();
    const auto misses = library_misses_.load();
    std::scoped_lock lock(library_mutex_);
    std::size_t decisions = 0;
    for (const auto& [id, cache] : library_cache_) decisions += cache.decisions.size();
    return Json{{"library",
                 {{"hits", hits},
                  {"misses", misses},
                  {"hit_rate", hits + misses == 0 ? 0.0 : static_cast<double>(hits) / static_cast<double>(hits + misses)},
                  {"profiles", library_cache_.size()},
                  {"decisions", decisions},
                  {"revision", store_.revision()}}},
                {"identity", {{"files", resolver_.cached()}}},
                {"details", {{"files", details_.cached()}}}};
}

Json Controller::command_stats_json() {
    Json rows = Json::array();
    std::scoped_lock lock(stats_mutex_);
    for (const auto& [name, stat] : command_stats_) {
        if (stat.count == 0) continue;
        const auto n = static_cast<double>(stat.count);
        rows.push_back({{"cmd", name},
                        {"count", stat.count},
                        {"run_avg_ms", static_cast<double>(stat.run_us) / n / 1000.0},
                        {"run_max_ms", static_cast<double>(stat.run_max_us) / 1000.0},
                        {"encode_avg_ms", static_cast<double>(stat.encode_us) / n / 1000.0},
                        {"bytes_avg", static_cast<double>(stat.bytes) / n},
                        {"bytes_max", stat.bytes_max}});
    }
    return rows;
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
    register_app_commands();
    register_catalog_commands();
}

}  // namespace houseguest::service
