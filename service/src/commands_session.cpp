#include <algorithm>

#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

namespace {
constexpr int kMaxSessionMinutes = 60 * 24 * 7;
}

void Controller::register_session_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("session.start", Access::Owner, [this](Who caller, Args args) {
        if (store_.state.session) throw Failure("conflict", "A session is already active.");
        Session session;
        session.profile_id = require_text(args, "profile_id");
        session.guest_sids = args.at("guest_sids").get<std::vector<std::string>>();
        if (session.guest_sids.empty() || session.guest_sids.size() > 16)
            throw Failure("bad_request", "Choose 1 to 16 guest accounts.");
        const auto users = system::list_local_users();
        for (const auto& sid : session.guest_sids) {
            if (std::ranges::none_of(users, [&](const system::LocalUser& u) { return u.sid == sid; })) {
                throw Failure("bad_request", "Unknown account.");
            }
        }
        const auto minutes = require_int(args, "duration_minutes", 1, kMaxSessionMinutes);
        session.started_at = now_seconds();
        session.ends_at = session.started_at + minutes * 60;
        session.on_expiry = args.value("on_expiry", store_.state.default_on_expiry);
        if (!valid_expiry_action(session.on_expiry)) throw Failure("bad_request", "Invalid expiry action.");
        session.audit_only = args.value("audit_only", false);

        const auto compiled =
            compile(find_profile(session.profile_id), {}, session.guest_sids, session.audit_only);
        if (compiled.has_errors)
            throw Failure("policy_invalid", "Policy has errors.", {{"findings", compiled.findings}});
        if (compiled.has_warnings && !args.value("confirm_warnings", false)) {
            throw Failure("confirmation_required", "Review the warnings before activating.",
                          {{"findings", compiled.findings}});
        }
        if (capabilities_.critical_failure()) {
            throw Failure("compatibility_blocked",
                          "A component Houseguest depends on is missing. Open Diagnostics for details.");
        }
        if (!session.audit_only && !supervisor_->running()) {
            throw Failure("enforcement_unavailable",
                          "Process supervision is not running, so restrictions cannot be enforced. "
                          "Use audit mode or open Diagnostics.");
        }
        arm(session);
        store_.state.session = session;
        store_.save();
        blocked_ = 0;
        audit_->append(caller.sid, "session.started",
                       {{"profile", session.profile_id},
                        {"minutes", minutes},
                        {"audit_only", session.audit_only},
                        {"guests", session.guest_sids}});
        timer_wake_.notify_all();
        return status_json();
    });

    add("session.stop", Access::Owner, [this](Who, Args) {
        end_session("stopped by owner", false);
        return status_json();
    });

    add("session.extend", Access::Owner, [this](Who caller, Args args) {
        if (!store_.state.session) throw Failure("conflict", "No active session.");
        const auto minutes = require_int(args, "minutes", 1, kMaxSessionMinutes);
        store_.state.session->ends_at += minutes * 60;
        store_.save();
        audit_->append(caller.sid, "session.extended", {{"minutes", minutes}});
        timer_wake_.notify_all();
        return status_json();
    });

    add("exceptions.add", Access::Owner, [this](Who caller, Args args) {
        if (!store_.state.session) throw Failure("conflict", "No active session.");
        const auto& app = args.at("app");
        const auto minutes = require_int(args, "minutes", 1, std::int64_t{60} * 24);
        const auto kind = args.value("match", "path");
        core::Rule rule;
        rule.id = random_id("exc-");
        rule.action = core::Action::Allow;
        rule.priority = 500;
        rule.expires_at = std::chrono::system_clock::now() + std::chrono::minutes(minutes);
        if (kind == "hash")
            rule.matcher = core::HashMatcher{require_text(app, "sha256")};
        else if (kind == "publisher")
            rule.matcher = core::PublisherMatcher{require_text(app, "publisher")};
        else
            rule.matcher = core::PathMatcher{require_text(app, "path")};

        auto session = *store_.state.session;
        session.exceptions.push_back(rule);
        const auto compiled = compile(find_profile(session.profile_id), session.exceptions,
                                      session.guest_sids, session.audit_only);
        if (compiled.has_errors)
            throw Failure("policy_invalid", "Exception is invalid.", {{"findings", compiled.findings}});
        supervisor_->apply(compiled.policy);
        store_.state.session = session;
        store_.save();
        audit_->append(caller.sid, "exception.added", {{"rule", core::to_json(rule)}});
        return core::to_json(rule);
    });
}

}  // namespace houseguest::service
