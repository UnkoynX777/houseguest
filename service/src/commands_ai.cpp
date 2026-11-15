#include <algorithm>

#include "houseguest/system/https.hpp"
#include "houseguest/system/text.hpp"

#include "ai/assistant.hpp"
#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

class Controller::AiBridge final : public ai::AssistantHost {
public:
    explicit AiBridge(Controller& c) : c_(c) {}

    Json read(const std::string& what, const Json& args) override {
        if (what == "apps") {
            Json list = Json::array();
            for (const auto& app : c_.catalog_.view().apps)
                if (app.role == system::AppRole::Application && !app.path.empty())
                    list.push_back({{"name", app.name}, {"path", app.path}, {"publisher", app.company}});
            return list;
        }
        std::scoped_lock lock(c_.mutex_);
        if (what == "status") return c_.status_json();
        if (what == "profiles") {
            Json rows = Json::array();
            for (const auto& p : c_.store_.state.profiles)
                rows.push_back({{"id", p.id},
                                {"name", p.name},
                                {"default", action_name(p.default_action)},
                                {"applications", p.apps.size()},
                                {"rules", p.rules.size()}});
            return rows;
        }
        if (what == "profile") {
            const auto& p = c_.find_profile(args.at("profile_id").get<std::string>());
            Json apps = Json::array();
            for (const auto& a : p.apps) {
                Json paths = Json::array();
                for (const auto& comp : a.components) paths.push_back(comp.path);
                apps.push_back({{"id", a.id}, {"name", a.name}, {"component_paths", paths}});
            }
            Json rules = Json::array();
            for (const auto& r : p.rules) {
                if (rules.size() >= 40) break;
                rules.push_back(core::to_json(r));
            }
            return Json{{"id", p.id},
                        {"name", p.name},
                        {"default", action_name(p.default_action)},
                        {"applications", apps},
                        {"rules", rules}};
        }
        if (what == "blocks") return c_.blocks(static_cast<std::size_t>(args.value("limit", 20)));
        if (what == "history") {
            AuditLog::Query query;
            query.limit = static_cast<std::size_t>(args.value("limit", 20));
            query.text = args.value("text", "");
            return c_.audit_->query(query);
        }
        if (what == "diagnostics") return c_.run_internal("diagnostics", Json::object());
        if (what == "explain") {
            return c_.run_internal("apps.explain",
                                   {{"profile_id", args.at("profile_id")}, {"path", args.at("path")}});
        }
        if (what == "simulate") {
            auto preview = c_.run_internal("policy.preview", {{"profile_id", args.at("profile_id")}});
            Json findings = Json::array();
            for (const auto& f : preview.value("findings", Json::array())) {
                if (findings.size() >= 10) break;
                findings.push_back(f);
            }
            return Json{{"allowed", preview.value("allowed", 0)},
                        {"blocked", preview.value("denied", 0)},
                        {"errors", preview.value("errors", false)},
                        {"warnings", preview.value("warnings", false)},
                        {"findings", findings}};
        }
        throw Failure("bad_request", "Unknown question.");
    }

    Json prepare(const Json& op) override {
        std::scoped_lock lock(c_.mutex_);
        return plan(op).preview;
    }

    Json apply(const Json& op, Json& rollback) override {
        std::scoped_lock lock(c_.mutex_);
        auto planned = plan(op);
        const auto owner = c_.store_.state.owner_sid;
        if (planned.created) {
            c_.store_profile("ai", planned.profile, "ai.profile_created");
            rollback = {{"kind", "delete_profile"}, {"id", planned.profile.id}};
        } else {
            rollback = {{"kind", "restore_profile"}, {"profile", core::to_json(planned.before)}};
            c_.store_profile("ai", planned.profile, "ai.profile_changed");
        }
        (void)owner;
        return Json{{"profile_id", planned.profile.id}, {"name", planned.profile.name}};
    }

    void undo(const Json& rollback) override {
        std::scoped_lock lock(c_.mutex_);
        const auto kind = rollback.value("kind", "");
        if (kind == "delete_profile") {
            const auto id = rollback.at("id").get<std::string>();
            if (c_.session_uses(id)) throw Failure("conflict", "This profile is used by the active session.");
            std::erase_if(c_.store_.state.profiles, [&](const core::Profile& p) { return p.id == id; });
            c_.store_.save();
        } else if (kind == "restore_profile") {
            c_.store_profile("ai", core::profile_from_json(rollback.at("profile")), "ai.profile_restored");
        } else {
            throw Failure("bad_request", "Nothing to undo.");
        }
    }

    void audit(const std::string& event, Json detail) override {
        c_.audit_->append("ai", event, std::move(detail));
    }

private:
    struct Plan {
        core::Profile profile, before;
        bool created = false;
        Json preview;
    };

    core::Application build_app(const std::string& path) {
        const auto& view = c_.catalog_.view();
        const auto it = std::ranges::find_if(view.apps, [&](const system::InstalledApp& a) {
            return a.role == system::AppRole::Application && a.path == path;
        });
        if (it == view.apps.end())
            throw Failure("not_found", "That program is not in the list of installed applications.");
        const auto identity = c_.resolver_.resolve(system::path_from_utf8(path), true, true);
        core::Component main;
        main.id = random_id("c-");
        main.label = it->name;
        main.role = core::ComponentRole::Main;
        main.scope = core::Scope::Independent;
        main.path = path;
        main.sha256 = identity.sha256;
        main.publisher = identity.publisher;
        main.package_family = it->package_family;
        main.follow_updates = !identity.publisher.empty();
        core::Application app;
        app.id = random_id("app-");
        app.name = it->name;
        app.mode = core::DependencyMode::Approved;
        app.unknown = core::UnknownPolicy::Review;
        app.priority = 50;
        app.components.push_back(std::move(main));
        return app;
    }

    Plan plan(const Json& op) {
        Plan result;
        const auto kind = op.value("kind", "");
        Json names = Json::array();
        if (kind == "create_profile") {
            result.created = true;
            result.profile.id = random_id("p-");
            result.profile.name = op.at("name").get<std::string>();
            result.profile.default_action = core::Action::Deny;
            if (std::ranges::any_of(c_.store_.state.profiles,
                                    [&](const core::Profile& p) { return p.name == result.profile.name; }))
                throw Failure("conflict", "A profile with that name already exists.");
            if (c_.store_.state.profiles.size() >= 40) throw Failure("limit", "There are too many profiles.");
            for (const auto& path : op.at("app_paths")) {
                auto app = build_app(path.get<std::string>());
                names.push_back(app.name);
                result.profile.apps.push_back(std::move(app));
            }
        } else {
            result.profile = c_.find_profile(op.at("profile_id").get<std::string>());
            result.before = result.profile;
            if (kind == "allow_app") {
                auto app = build_app(op.at("app_path").get<std::string>());
                if (std::ranges::any_of(result.profile.apps, [&](const core::Application& a) {
                        return std::ranges::any_of(a.components, [&](const core::Component& comp) {
                            return comp.path == app.components[0].path;
                        });
                    }))
                    throw Failure("conflict", "That program is already allowed in this profile.");
                names.push_back(app.name);
                result.profile.apps.push_back(std::move(app));
            } else if (kind == "remove_app") {
                const auto id = op.at("app_id").get<std::string>();
                const auto it = std::ranges::find(result.profile.apps, id, &core::Application::id);
                if (it == result.profile.apps.end())
                    throw Failure("not_found", "That application is not in the profile.");
                names.push_back(it->name);
                result.profile.apps.erase(it);
            } else {
                throw Failure("bad_request", "Unknown change.");
            }
        }
        const auto compiled = c_.compile(result.profile, {}, {}, false);
        if (compiled.has_errors)
            throw Failure("policy_invalid", "The resulting profile has errors.",
                          {{"findings", compiled.findings}});
        std::string summary;
        if (kind == "create_profile")
            summary = "Create the profile \"" + result.profile.name + "\" that blocks everything except " +
                      std::to_string(names.size()) + " program(s)";
        else if (kind == "allow_app")
            summary = "Allow " + names[0].get<std::string>() + " in \"" + result.profile.name + "\"";
        else
            summary = "Remove " + names[0].get<std::string>() + " from \"" + result.profile.name + "\"";
        const bool live = c_.store_.state.session && c_.store_.state.session->profile_id == result.profile.id;
        result.preview = Json{{"summary", summary},
                              {"profile", result.profile.name},
                              {"programs", names},
                              {"findings", compiled.findings},
                              {"affects_active_session", live},
                              {"default_action", action_name(result.profile.default_action)}};
        return result;
    }

    Controller& c_;
};

Json Controller::run_internal(const std::string& command, const Json& args) {
    const auto found = commands_.find(command);
    if (found == commands_.end()) throw Failure("not_found", "Unknown operation.");
    const Caller caller{store_.state.owner_sid, false};
    return found->second.run(caller, args);
}

ai::Assistant& Controller::assistant() {
    std::scoped_lock lock(ai_mutex_);
    if (!ai_) {
        ai_host_ = std::make_shared<AiBridge>(*this);
        ai_ = std::make_unique<ai::Assistant>(
            *ai_host_, store_.directory(),
            [](const system::HttpsRequest& request, const system::ChunkSink& sink,
               const std::atomic<bool>* cancel) { return system::https_request(request, sink, cancel); });
    }
    return *ai_;
}

void Controller::register_ai_commands() {
    using Args = const Json&;
    using Who = const Caller&;
    auto text = [](Args args, const char* key) {
        return args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : std::string();
    };

    add("ai.status", Access::Owner, [this](Who, Args) { return assistant().status(); }, true);
    add("ai.models", Access::Owner, [this](Who, Args) { return assistant().models(); }, true);
    add(
        "ai.key.set", Access::Owner,
        [this, text](Who caller, Args args) {
            auto result = assistant().set_key(text(args, "api_key"));
            audit_->append(caller.sid, "ai.key_set", {});
            return result;
        },
        true);
    add(
        "ai.key.clear", Access::Owner,
        [this](Who caller, Args) {
            assistant().clear_key();
            audit_->append(caller.sid, "ai.key_cleared", {});
            return Json::object();
        },
        true);
    add(
        "ai.settings.set", Access::Owner,
        [this](Who caller, Args args) {
            auto result = assistant().set_settings(args);
            audit_->append(caller.sid, "ai.settings", result);
            return result;
        },
        true);
    add(
        "ai.chat.send", Access::Owner,
        [this, text](Who, Args args) {
            const auto context =
                args.contains("context") && args["context"].is_object() ? args["context"] : Json::object();
            return Json{{"job", assistant().send(text(args, "chat_id"), text(args, "text"), context)}};
        },
        true);
    add(
        "ai.job", Access::Owner,
        [this, text](Who, Args args) {
            return assistant().job(text(args, "id"), static_cast<std::size_t>(args.value("since", 0)));
        },
        true);
    add(
        "ai.cancel", Access::Owner,
        [this, text](Who, Args args) {
            assistant().cancel(text(args, "id"));
            return Json::object();
        },
        true);
    add("ai.chat.list", Access::Owner, [this](Who, Args) { return assistant().chats(); }, true);
    add(
        "ai.chat.get", Access::Owner,
        [this, text](Who, Args args) { return assistant().chat(text(args, "id")); }, true);
    add(
        "ai.chat.delete", Access::Owner,
        [this, text](Who, Args args) {
            assistant().delete_chat(text(args, "id"));
            return Json::object();
        },
        true);
    add("ai.proposals", Access::Owner, [this](Who, Args) { return assistant().proposals(); }, true);
    add(
        "ai.proposal.apply", Access::Owner,
        [this, text](Who, Args args) { return assistant().apply_proposal(text(args, "id")); }, true);
    add(
        "ai.proposal.discard", Access::Owner,
        [this, text](Who, Args args) {
            assistant().discard_proposal(text(args, "id"));
            return Json::object();
        },
        true);
    add(
        "ai.proposal.rollback", Access::Owner,
        [this, text](Who, Args args) { return assistant().rollback_proposal(text(args, "id")); }, true);
}

}  // namespace houseguest::service
