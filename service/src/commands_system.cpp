#include <algorithm>

#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

void Controller::register_system_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("dashboard", Access::Owner, [this](Who, Args) {
        auto data = status_json();
        data["blocked"] = blocked_.load();
        data["profiles"] = store_.state.profiles.size();
        if (const auto& session = store_.state.session) {
            const auto& profile = find_profile(session->profile_id);
            data["session"] = {{"profile_id", profile.id},          {"profile_name", profile.name},
                               {"started_at", session->started_at}, {"ends_at", session->ends_at},
                               {"audit_only", session->audit_only}, {"guest_sids", session->guest_sids}};
            data["allowed_rules"] = std::ranges::count_if(
                profile.rules, [](const core::Rule& r) { return r.action == core::Action::Allow; });
            Json exceptions = Json::array();
            for (const auto& rule : session->exceptions) exceptions.push_back(core::to_json(rule));
            data["exceptions"] = std::move(exceptions);
        }
        data["recent"] = audit_->tail(8);
        return data;
    });

    add("audit.list", Access::Owner, [this](Who, Args args) {
        return audit_->tail(static_cast<std::size_t>(require_int(args, "limit", 1, 1000)));
    });

    add("audit.query", Access::Owner, [this](Who, Args args) {
        AuditLog::Query query;
        query.limit =
            static_cast<std::size_t>(args.contains("limit") ? require_int(args, "limit", 1, 200) : 50);
        if (args.contains("before"))
            query.before = static_cast<std::uint64_t>(require_int(args, "before", 0, INT64_MAX));
        if (args.contains("since")) query.since = require_int(args, "since", 0, INT64_MAX);
        if (args.contains("text")) query.text = require_text(args, "text").substr(0, 120);
        if (args.contains("events") && args["events"].is_array()) {
            for (const auto& name : args["events"]) {
                if (name.is_string() && query.events.size() < 64)
                    query.events.push_back(name.get<std::string>());
            }
        }
        return audit_->query(query);
    });

    add("audit.verify", Access::Owner, [this](Who, Args) {
        const auto report = audit_->verify();
        return Json{{"valid", report.valid},
                    {"truncated", report.truncated},
                    {"entries", report.entries},
                    {"problem", report.problem}};
    });

    add("diagnostics", Access::Owner, [this](Who, Args) {
        capabilities_ = system::probe_capabilities();
        return Json{{"version", kVersion},
                    {"capabilities", system::to_json(capabilities_)},
                    {"supervisor_running", supervisor_->running()},
                    {"supervisor", supervisor_json()},
                    {"audit",
                     [&] {
                         const auto report = audit_->verify();
                         return Json{{"valid", report.valid},
                                     {"truncated", report.truncated},
                                     {"entries", report.entries},
                                     {"problem", report.problem},
                                     {"write_failures", audit_->write_failures()}};
                     }()},
                    {"audit_chain_valid", audit_->verify_chain()},
                    {"ipc", ipc_stats_ ? ipc_stats_() : Json::object()},
                    {"state", load_result_ == LoadResult::Loaded      ? "loaded"
                              : load_result_ == LoadResult::Fresh     ? "fresh"
                              : load_result_ == LoadResult::Recovered ? "recovered"
                                                                      : "tampered"},
                    {"data_directory", system::path_to_utf8(store_.directory())},
                    {"install_directory", system::path_to_utf8(install_dir_)},
                    {"backups", store_.list_backups().size()},
                    {"uptime_seconds", std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() - started_)
                                           .count()}};
    });

    add("backup.create", Access::Owner, [this](Who caller, Args) {
        const auto file = store_.create_backup();
        audit_->append(caller.sid, "backup.created", {{"file", system::path_to_utf8(file.filename())}});
        return Json{{"name", system::path_to_utf8(file.filename())}};
    });

    add("backup.list", Access::Owner, [this](Who, Args) { return Json(store_.list_backups()); });

    add("backup.restore", Access::Owner, [this](Who caller, Args args) {
        if (store_.state.session)
            throw Failure("conflict", "End the active session before restoring a backup.");
        const auto owner = store_.state.owner_sid;
        const auto pin = store_.state.pin;
        if (!store_.restore_backup(require_text(args, "name")))
            throw Failure("bad_request", "Backup is missing or failed its integrity check.");
        store_.state.owner_sid = owner;
        store_.state.pin = pin;
        store_.save();
        audit_->append(caller.sid, "backup.restored", {{"name", args["name"]}});
        return Json::object();
    });
}

}  // namespace houseguest::service
