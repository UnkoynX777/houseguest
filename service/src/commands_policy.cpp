#include <algorithm>
#include <set>

#include "houseguest/core/analysis.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

void Controller::register_policy_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("profiles.list", Access::Owner, [this](Who, Args) {
        Json list = Json::array();
        for (const auto& profile : store_.state.profiles) list.push_back(core::to_json(profile));
        return list;
    });

    add("profiles.save", Access::Owner, [this](Who caller, Args args) {
        auto profile = core::profile_from_json(args.at("profile"));
        store_profile(caller.sid, profile, "profile.saved");
        return core::to_json(profile);
    });

    add("profiles.import", Access::Owner, [this](Who caller, Args args) {
        auto profile = core::profile_from_json(args.at("profile"));
        profile.id = random_id("p-");
        store_.state.profiles.push_back(profile);
        store_.save();
        audit_->append(caller.sid, "profile.imported", {{"profile", profile.id}, {"name", profile.name}});
        return core::to_json(profile);
    });

    add("profiles.delete", Access::Owner, [this](Who caller, Args args) {
        const auto id = require_text(args, "id");
        if (session_uses(id)) throw Failure("conflict", "This profile is used by the active session.");
        find_profile(id);
        std::erase_if(store_.state.profiles, [&](const core::Profile& p) { return p.id == id; });
        store_.save();
        audit_->append(caller.sid, "profile.deleted", {{"profile", id}});
        return Json::object();
    });

    add(
        "apps.discover", Access::Owner,
        [](Who, Args) {
            Json list = Json::array();
            for (const auto& app : system::discover_installed_apps()) {
                list.push_back({{"name", app.name},
                                {"path", app.path},
                                {"company", app.company},
                                {"version", app.version},
                                {"description", app.description},
                                {"source", app.source},
                                {"package_family", app.package_family}});
            }
            return list;
        },
        true);

    add(
        "apps.inspect", Access::Owner,
        [this](Who, Args args) {
            const auto path = system::path_from_utf8(require_text(args, "path"));
            std::error_code ec;
            if (!path.is_absolute() || !std::filesystem::is_regular_file(path, ec))
                throw Failure("not_found", "File not found.");
            auto identity = resolver_.resolve(path, true, true);
            auto json = core::to_json(identity);
            json["size"] = std::filesystem::file_size(path, ec);
            return json;
        },
        true);

    add(
        "users.list", Access::Owner,
        [this](Who, Args) {
            std::string owner;
            std::set<std::string> managed;
            {
                std::scoped_lock lock(mutex_);
                owner = store_.state.owner_sid;
                for (const auto& account : store_.state.managed) managed.insert(account.sid);
            }
            Json list = Json::array();
            for (const auto& user : system::list_local_users()) {
                const bool is_managed_account = managed.contains(user.sid);
                if (user.disabled && !is_managed_account) continue;
                list.push_back({{"name", user.name},
                                {"sid", user.sid},
                                {"owner", user.sid == owner},
                                {"managed", is_managed_account}});
            }
            return list;
        },
        true);

    add(
        "policy.preview", Access::Owner,
        [this](Who, Args args) {
            core::Profile profile;
            std::vector<std::string> guests;
            Compiled compiled;
            {
                std::scoped_lock lock(mutex_);
                profile = args.contains("profile") ? core::profile_from_json(args["profile"])
                                                   : find_profile(require_text(args, "profile_id"));
                guests = args.value("guest_sids", std::vector<std::string>{});
                compiled = compile(profile, {}, guests, false);
            }
            Json result{{"findings", compiled.findings},
                        {"errors", compiled.has_errors},
                        {"warnings", compiled.has_warnings}};

            Json apps = Json::array();
            std::size_t allowed = 0, denied = 0;
            for (const auto& app : system::discover_installed_apps()) {
                const auto decision =
                    decide_file(*compiled.policy, system::path_from_utf8(app.path), app.package_family, {});
                (decision.action == core::Action::Allow ? allowed : denied)++;
                apps.push_back({{"name", app.name},
                                {"path", app.path},
                                {"action", action_name(decision.action)},
                                {"rule", decision.rule_id},
                                {"app_id", decision.app_id},
                                {"reason", decision.reason},
                                {"source", core::source_name(decision.source)}});
            }
            result["applications"] = profile.apps.size();
            result["continuity"] = continuity_.report_json(guests);
            result["allowed"] = allowed;
            result["denied"] = denied;
            result["apps"] = std::move(apps);
            return result;
        },
        true);
}

}  // namespace houseguest::service
