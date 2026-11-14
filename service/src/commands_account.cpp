#include <algorithm>

#include "houseguest/system/accounts.hpp"
#include "houseguest/system/crypto.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

void Controller::register_account_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("ping", Access::Anyone, [](Who, Args) { return Json{{"version", kVersion}}; });
    add("status", Access::Anyone, [this](Who, Args) { return status_json(); });

    add("setup", Access::Elevated, [this](Who caller, Args args) {
        if (store_.state.configured()) throw Failure("conflict", "Houseguest is already configured.");
        auto& state = store_.state;
        state.owner_sid = caller.sid;
        set_pin(require_text(args, "pin"));
        state.profiles = core::builtin_profiles();
        store_.save();
        audit_->append(caller.sid, "setup", {});
        return Json{{"token", issue_token(caller.sid)}};
    });

    add("unlock", Access::Anyone, [this](Who caller, Args args) {
        if (!store_.state.configured()) throw Failure("not_configured", "Run setup first.");
        if (caller.sid != store_.state.owner_sid) {
            audit_->append(caller.sid, "unlock.denied", {{"reason", "not the owner account"}});
            throw Failure("forbidden", "Only the owner account can unlock Houseguest.");
        }
        verify_pin(require_text(args, "pin"));
        audit_->append(caller.sid, "unlock", {});
        return Json{{"token", issue_token(caller.sid)}};
    });

    add("lock", Access::Owner, [this](Who, Args) {
        std::scoped_lock tokens(token_mutex_);
        tokens_.clear();
        return Json::object();
    });

    add("recover", Access::Elevated, [this](Who caller, Args args) {
        end_session("administrator recovery", false);
        if (store_.state.session) force_end_session("administrator recovery could not finish cleanly");
        const bool reset = args.value("reset_owner", false);
        if (reset) {
            store_.state.owner_sid.clear();
            store_.state.pin = {};
            store_.state.failed_attempts = 0;
            store_.state.locked_until = 0;
            {
                std::scoped_lock tokens(token_mutex_);
                tokens_.clear();
            }
            store_.save();
        }
        audit_->append(caller.sid, "recover", {{"reset_owner", reset}});
        return Json{{"reset_owner", reset}};
    });

    add("settings.get", Access::Owner, [this](Who, Args) {
        return Json{{"language", store_.state.language},
                    {"default_on_expiry", store_.state.default_on_expiry}};
    });

    add("settings.set", Access::Owner, [this](Who caller, Args args) {
        if (args.contains("language")) {
            const auto language = require_text(args, "language");
            if (language != "en" && language != "pt-BR")
                throw Failure("bad_request", "Unsupported language.");
            store_.state.language = language;
        }
        if (args.contains("default_on_expiry")) {
            const auto action = require_text(args, "default_on_expiry");
            if (!valid_expiry_action(action) || action == "disable")
                throw Failure("bad_request", "Invalid expiry action.");
            store_.state.default_on_expiry = action;
        }
        store_.save();
        audit_->append(caller.sid, "settings.changed", args);
        return Json{{"language", store_.state.language},
                    {"default_on_expiry", store_.state.default_on_expiry}};
    });

    add("accounts.managed.list", Access::Owner, [this](Who, Args) {
        Json list = Json::array();
        for (const auto& account : store_.state.managed) {
            list.push_back(
                {{"name", account.name},
                 {"sid", account.sid},
                 {"exists", system::user_exists(account.sid)},
                 {"in_session",
                  store_.state.session && std::ranges::find(store_.state.session->guest_sids, account.sid) !=
                                              store_.state.session->guest_sids.end()}});
        }
        return list;
    });

    add("accounts.managed.create", Access::Owner, [this](Who caller, Args args) {
        const auto name = "hg-" + require_text(args, "name");
        if (!system::valid_managed_name(name))
            throw Failure("bad_request", "Use up to 13 letters, digits, - or _.");
        if (store_.state.managed.size() >= 8)
            throw Failure("conflict", "Remove an unused managed account first.");
        std::string password = args.value("password", std::string());
        const bool generated = password.empty();
        if (generated) password = system::to_hex(system::random_bytes(12));
        if (password.size() < 8 || password.size() > 127)
            throw Failure("bad_request", "Password must have 8 to 127 characters.");
        std::string error;
        const auto created = system::create_standard_user(name, password, error);
        if (!created) throw Failure("account_error", error);
        store_.state.managed.push_back({created->name, created->sid});
        store_.save();
        audit_->append(caller.sid, "managed_account.created",
                       {{"name", created->name}, {"sid", created->sid}});
        Json reply{{"name", created->name}, {"sid", created->sid}};
        if (generated) reply["password"] = password;
        return reply;
    });

    add("accounts.managed.delete", Access::Owner, [this](Who caller, Args args) {
        const auto sid = require_text(args, "sid");
        const auto it = std::ranges::find(store_.state.managed, sid, &ManagedAccount::sid);
        if (it == store_.state.managed.end()) throw Failure("not_found", "Not a Houseguest managed account.");
        if (store_.state.session && std::ranges::find(store_.state.session->guest_sids, sid) !=
                                        store_.state.session->guest_sids.end())
            throw Failure("conflict", "This account is used by the active session.");
        system::disconnect_sessions({sid}, true);
        if (system::user_exists(sid) && !system::delete_user(sid))
            throw Failure("account_error", "The account could not be removed.");
        const auto name = it->name;
        store_.state.managed.erase(it);
        store_.save();
        audit_->append(caller.sid, "managed_account.deleted", {{"name", name}, {"sid", sid}});
        return Json::object();
    });

    add("pin.change", Access::Owner, [this](Who caller, Args args) {
        verify_pin(require_text(args, "old"));
        set_pin(require_text(args, "new"));
        store_.save();
        audit_->append(caller.sid, "pin.changed", {});
        return Json::object();
    });
}

}  // namespace houseguest::service
