#include "houseguest/system/text.hpp"

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
        tokens_.clear();
        return Json::object();
    });

    add("recover", Access::Elevated, [this](Who caller, Args args) {
        end_session("administrator recovery", false);
        const bool reset = args.value("reset_owner", false);
        if (reset) {
            store_.state.owner_sid.clear();
            store_.state.pin = {};
            store_.state.failed_attempts = 0;
            store_.state.locked_until = 0;
            tokens_.clear();
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
            if (!valid_expiry_action(action)) throw Failure("bad_request", "Invalid expiry action.");
            store_.state.default_on_expiry = action;
        }
        store_.save();
        audit_->append(caller.sid, "settings.changed", args);
        return Json{{"language", store_.state.language},
                    {"default_on_expiry", store_.state.default_on_expiry}};
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
