#include <algorithm>
#include <set>

#include "houseguest/core/analysis.hpp"
#include "houseguest/core/glob.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

namespace {

Json decision_json(const core::Decision& d) {
    Json json{{"action", action_name(d.action)},
              {"source", core::source_name(d.source)},
              {"reason", d.reason},
              {"rule_id", d.rule_id},
              {"app_id", d.app_id},
              {"component_id", d.component_id},
              {"note", d.note},
              {"review_app", d.review_app}};
    return json;
}

bool has_wildcard(const std::string& path) {
    return path.find_first_of("*?") != std::string::npos;
}

std::string lowered(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const char* state_name(core::MatchState state) {
    switch (state) {
        case core::MatchState::Exact:
            return "approved";
        case core::MatchState::Updated:
            return "updated";
        case core::MatchState::Changed:
            return "changed";
        default:
            return "unknown";
    }
}

core::Component component_for(const core::AppIdentity& identity, core::ComponentRole role, core::Scope scope,
                              const std::string& label) {
    core::Component c;
    c.id = random_id("c-");
    c.label = label.empty() ? system::path_from_utf8(identity.path).filename().string() : label;
    c.role = role;
    c.scope = scope;
    c.path = identity.path;
    c.sha256 = identity.sha256;
    c.publisher = identity.publisher;
    c.package_family = identity.package_family;
    c.follow_updates = !identity.publisher.empty();
    return c;
}

core::ComponentRole role_from(const std::string& text) {
    if (text == "launcher") return core::ComponentRole::Launcher;
    if (text == "helper") return core::ComponentRole::Helper;
    if (text == "runtime") return core::ComponentRole::Runtime;
    if (text == "updater") return core::ComponentRole::Updater;
    if (text == "service") return core::ComponentRole::Service;
    if (text == "main") return core::ComponentRole::Main;
    return core::ComponentRole::Component;
}

core::Application* find_application(core::Profile& profile, const std::string& id) {
    const auto it = std::ranges::find(profile.apps, id, &core::Application::id);
    return it == profile.apps.end() ? nullptr : &*it;
}

}  // namespace

core::Decision Controller::decide_file(const EnforcementPolicy& policy, const std::filesystem::path& file,
                                       const std::string& package_family, std::vector<std::string> lineage) {
    core::ProcessFacts facts;
    facts.lineage = std::move(lineage);
    auto first = resolver_.resolve_checked(file, false, false, package_family);
    facts.app = std::move(first.identity);
    facts.remote_image = first.remote;
    core::Decision decision;
    const auto now = std::chrono::system_clock::now();
    for (int round = 0; round < 3; ++round) {
        decision = core::evaluate(policy.profile, policy.protection, facts, now);
        const bool need_hash = decision.needs_hash && !facts.hash_resolved;
        const bool need_publisher = decision.needs_publisher && !facts.publisher_resolved;
        if (!need_hash && !need_publisher) break;
        if (!facts.remote_image) {
            auto full = resolver_.resolve_checked(file, need_hash || facts.hash_resolved,
                                                  need_publisher || facts.publisher_resolved, package_family);
            facts.app = std::move(full.identity);
        }
        facts.hash_resolved = facts.hash_resolved || need_hash;
        facts.publisher_resolved = facts.publisher_resolved || need_publisher;
    }
    return decision;
}

void Controller::store_profile(const std::string& actor, core::Profile profile, const char* event) {
    const auto it = std::ranges::find(store_.state.profiles, profile.id, &core::Profile::id);
    const bool active = session_uses(profile.id);
    if (active && store_.state.session->ending())
        throw Failure("conflict", "The session is ending. Try again in a moment.");
    if (active) {
        const auto& session = *store_.state.session;
        const auto compiled = compile(profile, session.exceptions, session.guest_sids, session.audit_only);
        if (compiled.has_errors)
            throw Failure("policy_invalid", "Policy has errors.", {{"findings", compiled.findings}});
        continuity_.restrict_to(session.guest_sids);
        supervisor_->apply(compiled.policy);
        if (supervisor_->running()) supervisor_->sweep();
    }
    if (it == store_.state.profiles.end())
        store_.state.profiles.push_back(profile);
    else
        *it = profile;
    store_.save();
    audit_->append(actor, event,
                   {{"profile", profile.id},
                    {"name", profile.name},
                    {"rules", profile.rules.size()},
                    {"apps", profile.apps.size()},
                    {"live", active}});
}

Json Controller::blocks(std::size_t limit) {
    Json result = Json::array();
    const auto entries = audit_->tail(1000);
    for (const auto& entry : entries) {
        const auto event = entry.value("event", "");
        if (event != "app.blocked" && event != "app.would_block" && event != "app.block_failed" &&
            event != "protection.spared")
            continue;
        Json item = entry.value("detail", Json::object());
        item["event"] = event;
        item["ts"] = entry.value("ts", std::int64_t{0});
        result.push_back(std::move(item));
        if (result.size() >= limit) break;
    }
    return result;
}

Json Controller::explain(const core::Profile& profile, const std::filesystem::path& file,
                         const std::string& package_family) {
    const auto compiled = compile(profile, {}, {}, false);
    const auto& policy = *compiled.policy;
    const auto decision = decide_file(policy, file, package_family, {});
    const auto identity = resolver_.resolve(file, true, true);

    Json result{{"path", system::path_to_utf8(file)},
                {"decision", decision_json(decision)},
                {"sha256", identity.sha256},
                {"publisher", identity.publisher}};

    const core::Application* listed = nullptr;
    for (const auto& app : policy.profile.apps) {
        if (app.id == decision.app_id && !decision.app_id.empty()) listed = &app;
    }
    if (!listed) {
        for (const auto& app : policy.profile.apps) {
            for (const auto& c : app.components) {
                if (c.role == core::ComponentRole::Main && !c.path.empty() && !has_wildcard(c.path) &&
                    core::normalize_path(c.path) == core::normalize_path(system::path_to_utf8(file)))
                    listed = &app;
            }
        }
    }

    std::size_t approved = 0, changed = 0, missing = 0, unpinned = 0;
    Json components = Json::array();
    if (listed) {
        const auto now = std::chrono::system_clock::now();
        for (const auto& c : listed->components) {
            Json row = to_json(c);
            std::string state = "unknown";
            Json effective = nullptr;
            if (!core::component_is_active(c, now)) {
                state = "expired";
            } else if (has_wildcard(c.path) || c.path.empty()) {
                state = "pattern";
            } else {
                std::error_code ec;
                const auto target = system::path_from_utf8(c.path);
                if (!std::filesystem::is_regular_file(target, ec)) {
                    state = "missing";
                    ++missing;
                } else {
                    core::ProcessFacts facts;
                    facts.app = resolver_.resolve(target, true, true);
                    facts.app.package_family = c.package_family;
                    facts.hash_resolved = facts.publisher_resolved = true;
                    facts.lineage = {listed->id};
                    state = state_name(core::match_component(c, facts));
                    if (state == "unknown") state = "changed";
                    row["on_disk"] = {{"sha256", facts.app.sha256}, {"publisher", facts.app.publisher}};
                    effective = decision_json(core::evaluate(policy.profile, policy.protection, facts, now));
                    if (state == "changed")
                        ++changed;
                    else if (c.sha256.empty() && c.publisher.empty() && c.package_family.empty()) {
                        state = "unpinned";
                        ++unpinned;
                    } else {
                        ++approved;
                    }
                }
            }
            row["state"] = state;
            row["effective"] = std::move(effective);
            components.push_back(std::move(row));
        }
        result["application"] = {{"id", listed->id},
                                 {"name", listed->name},
                                 {"mode", to_json(*listed)["mode"]},
                                 {"unknown", to_json(*listed)["unknown"]},
                                 {"priority", listed->priority}};
    }
    result["components"] = std::move(components);

    Json pending = Json::array();
    {
        std::scoped_lock lock(review_mutex_);
        for (const auto& [id, review] : reviews_) {
            if (listed && review.app_id == listed->id && review.profile_id == profile.id)
                pending.push_back({{"id", review.id},
                                   {"kind", review.kind},
                                   {"path", review.path},
                                   {"sha256", review.sha256},
                                   {"publisher", review.publisher},
                                   {"count", review.count},
                                   {"last", review.last}});
        }
    }
    result["summary"] = {{"approved", approved},
                         {"changed", changed},
                         {"missing", missing},
                         {"unpinned", unpinned},
                         {"pending", pending.size()}};
    result["pending"] = std::move(pending);

    Json history = Json::array();
    const auto path_key = core::normalize_path(system::path_to_utf8(file));
    for (const auto& block : blocks(300)) {
        const bool same_file = core::normalize_path(block.value("path", "")) == path_key;
        const bool same_app = listed && block.value("related_app", "") == listed->id;
        if ((same_file || same_app) && history.size() < 30) history.push_back(block);
    }
    result["history"] = std::move(history);

    Json findings = Json::array();
    for (const auto& finding : compiled.findings) {
        if (!listed) break;
        const auto& ids = finding["rules"];
        if (!ids.empty() && ids.front() == listed->id) findings.push_back(finding);
    }
    result["findings"] = std::move(findings);
    return result;
}

Json Controller::approve_component(const std::string& actor, const std::string& profile_id,
                                   const std::string& app_id, const std::string& path, const Json& args) {
    auto profile = find_profile(profile_id);
    auto* app = find_application(profile, app_id);
    if (!app) throw Failure("not_found", "The application is no longer in the profile.");
    const auto file = system::path_from_utf8(path);
    std::error_code ec;
    if (!file.is_absolute() || !std::filesystem::is_regular_file(file, ec))
        throw Failure("not_found", "The file no longer exists.");
    const auto identity = resolver_.resolve(file, true, true);
    if (identity.sha256.empty()) throw Failure("not_found", "The file could not be read.");
    if (args.contains("expect_sha256") && args["expect_sha256"] != identity.sha256)
        throw Failure("conflict", "The file changed after it was shown. Review it once more.");
    if (core::is_sensitive_tool(identity.path) && !args.value("acknowledge_sensitive", false))
        throw Failure("confirmation_required",
                      "This is a tool that can run other programs. Confirm to approve it.");

    const auto minutes = args.value("minutes", std::int64_t{0});
    if (minutes < 0 || minutes > 60 * 24 * 30) throw Failure("bad_request", "Invalid duration.");
    const auto existing = std::ranges::find_if(app->components, [&](const core::Component& c) {
        return core::normalize_path(c.path) == core::normalize_path(identity.path);
    });
    if (existing != app->components.end()) {
        existing->sha256 = identity.sha256;
        if (!identity.publisher.empty()) existing->publisher = identity.publisher;
    } else {
        auto component = component_for(identity, role_from(args.value("role", std::string{"helper"})),
                                       args.value("scope", std::string{"descendant"}) == "independent"
                                           ? core::Scope::Independent
                                           : core::Scope::Descendant,
                                       {});
        if (minutes > 0)
            component.expires_at = std::chrono::system_clock::now() + std::chrono::minutes(minutes);
        app->components.push_back(std::move(component));
    }
    store_profile(actor, profile, "app.dependency_approved");
    return Json{{"approved", identity.path}};
}

void Controller::register_app_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add(
        "apps.library", Access::Owner,
        [this](Who, Args args) {
            core::Profile profile;
            {
                std::scoped_lock lock(mutex_);
                profile = find_profile(require_text(args, "profile_id"));
            }
            const auto compiled = compile(profile, {}, {}, false);
            const auto recent = blocks(500);
            std::map<std::string, std::uint64_t> blocked_by_path;
            for (const auto& block : recent) ++blocked_by_path[core::normalize_path(block.value("path", ""))];
            std::map<std::string, std::uint64_t> pending_by_app;
            {
                std::scoped_lock lock(review_mutex_);
                for (const auto& [id, review] : reviews_) {
                    if (review.profile_id == profile.id) ++pending_by_app[review.app_id];
                }
            }
            std::map<std::string, std::uint64_t> conflicts;
            for (const auto& finding : compiled.findings) {
                if (finding["code"] == "application_blocked_by_rule" && !finding["rules"].empty())
                    ++conflicts[finding["rules"].front().get<std::string>()];
            }

            Json list = Json::array();
            std::size_t allowed = 0, blocked = 0;
            for (const auto& installed : system::discover_installed_apps()) {
                const auto file = system::path_from_utf8(installed.path);
                const auto decision = decide_file(*compiled.policy, file, installed.package_family, {});
                const bool allow = decision.action == core::Action::Allow;
                (allow ? allowed : blocked)++;
                const auto key = core::normalize_path(installed.path);
                Json row{{"name", installed.name},
                         {"path", installed.path},
                         {"company", installed.company},
                         {"version", installed.version},
                         {"description", installed.description},
                         {"source", installed.source},
                         {"package_family", installed.package_family},
                         {"status", allow ? "allowed" : "blocked"},
                         {"via", core::source_name(decision.source)},
                         {"reason", decision.reason},
                         {"rule_id", decision.rule_id},
                         {"app_id", decision.app_id},
                         {"blocks", blocked_by_path.contains(key) ? blocked_by_path[key] : 0}};
                for (const auto& app : compiled.policy->profile.apps) {
                    const bool is_main = std::ranges::any_of(app.components, [&](const core::Component& c) {
                        return c.role == core::ComponentRole::Main &&
                               ((!c.package_family.empty() && c.package_family == installed.package_family) ||
                                (!c.path.empty() && !has_wildcard(c.path) &&
                                 core::normalize_path(c.path) == key));
                    });
                    if (!is_main) continue;
                    row["listed"] = app.id;
                    row["dependencies"] = app.components.size() - 1;
                    row["pending"] = pending_by_app.contains(app.id) ? pending_by_app[app.id] : 0;
                    row["conflicts"] = conflicts.contains(app.id) ? conflicts[app.id] : 0;
                    break;
                }
                list.push_back(std::move(row));
            }
            return Json{{"apps", std::move(list)}, {"allowed", allowed}, {"blocked", blocked}};
        },
        true);

    add(
        "apps.explain", Access::Owner,
        [this](Who, Args args) {
            core::Profile profile;
            {
                std::scoped_lock lock(mutex_);
                profile = find_profile(require_text(args, "profile_id"));
            }
            const auto file = system::path_from_utf8(require_text(args, "path"));
            std::error_code ec;
            if (!file.is_absolute() || !std::filesystem::is_regular_file(file, ec))
                throw Failure("not_found", "File not found.");
            return explain(profile, file, args.value("package_family", std::string{}));
        },
        true);

    add("apps.save", Access::Owner, [this](Who caller, Args args) {
        auto profile = find_profile(require_text(args, "profile_id"));
        auto app = core::application_from_json(args.at("app"));
        if (app.id.empty()) app.id = random_id("app-");
        if (auto* existing = find_application(profile, app.id))
            *existing = app;
        else
            profile.apps.push_back(app);
        store_profile(caller.sid, profile, "app.saved");
        return Json{{"app", core::to_json(app)}, {"findings", compile(profile, {}, {}, false).findings}};
    });

    add("apps.remove", Access::Owner, [this](Who caller, Args args) {
        auto profile = find_profile(require_text(args, "profile_id"));
        const auto id = require_text(args, "app_id");
        if (!find_application(profile, id)) throw Failure("not_found", "Application not found.");
        std::erase_if(profile.apps, [&](const core::Application& a) { return a.id == id; });
        store_profile(caller.sid, profile, "app.removed");
        return Json::object();
    });

    add("apps.reauthorize", Access::Owner, [this](Who caller, Args args) {
        auto profile = find_profile(require_text(args, "profile_id"));
        auto* app = find_application(profile, require_text(args, "app_id"));
        if (!app) throw Failure("not_found", "Application not found.");
        const auto component_id = require_text(args, "component_id");
        const auto it = std::ranges::find(app->components, component_id, &core::Component::id);
        if (it == app->components.end() || has_wildcard(it->path) || it->path.empty())
            throw Failure("bad_request", "This component cannot be re-pinned.");
        const auto identity = resolver_.resolve(system::path_from_utf8(it->path), true, true);
        if (identity.sha256.empty()) throw Failure("not_found", "The file could not be read.");
        if (args.contains("expect_sha256") && args["expect_sha256"] != identity.sha256)
            throw Failure("conflict", "The file changed again. Review it once more.");
        it->sha256 = identity.sha256;
        if (!identity.publisher.empty()) it->publisher = identity.publisher;
        store_profile(caller.sid, profile, "app.component_reauthorized");
        return core::to_json(*it);
    });

    add("library.list", Access::Owner, [this](Who, Args) {
        Json list = Json::array();
        for (const auto& app : store_.state.library) list.push_back(core::to_json(app));
        return list;
    });

    add("library.save", Access::Owner, [this](Who caller, Args args) {
        auto app = core::application_from_json(args.at("app"));
        if (store_.state.library.size() >= 200 &&
            std::ranges::none_of(store_.state.library, [&](const auto& a) { return a.id == app.id; }))
            throw Failure("conflict", "The library is full.");
        const auto it = std::ranges::find(store_.state.library, app.id, &core::Application::id);
        if (it == store_.state.library.end())
            store_.state.library.push_back(app);
        else
            *it = app;
        store_.save();
        audit_->append(caller.sid, "library.saved", {{"app", app.id}, {"name", app.name}});
        return core::to_json(app);
    });

    add("library.delete", Access::Owner, [this](Who caller, Args args) {
        const auto id = require_text(args, "id");
        std::erase_if(store_.state.library, [&](const core::Application& a) { return a.id == id; });
        store_.save();
        audit_->append(caller.sid, "library.deleted", {{"app", id}});
        return Json::object();
    });

    add("library.apply", Access::Owner, [this](Who caller, Args args) {
        auto profile = find_profile(require_text(args, "profile_id"));
        const auto it =
            std::ranges::find(store_.state.library, require_text(args, "id"), &core::Application::id);
        if (it == store_.state.library.end()) throw Failure("not_found", "Dependency set not found.");
        auto app = *it;
        if (find_application(profile, app.id)) app.id = random_id("app-");
        profile.apps.push_back(app);
        store_profile(caller.sid, profile, "app.saved");
        return core::to_json(app);
    });

    add("discovery.start", Access::Owner, [this](Who caller, Args args) {
        Discovery::Target target;
        target.label = args.value("label", std::string{});
        target.path = args.value("path", std::string{});
        target.package_family = args.value("package_family", std::string{});
        target.minutes = args.value("minutes", std::int64_t{10});
        target.user_sid = args.value("user_sid", caller.sid);
        if (target.path.empty() && target.package_family.empty())
            throw Failure("bad_request", "Choose an application to observe.");
        if (!target.path.empty()) {
            std::error_code ec;
            const auto file = system::path_from_utf8(target.path);
            if (!file.is_absolute() || !std::filesystem::is_regular_file(file, ec))
                throw Failure("not_found", "File not found.");
            target.path = system::path_to_utf8(file);
        }
        const auto users = system::list_local_users();
        if (std::ranges::none_of(users, [&](const system::LocalUser& u) { return u.sid == target.user_sid; }))
            throw Failure("bad_request", "Unknown account.");
        if (!supervisor_->running())
            throw Failure("enforcement_unavailable",
                          "Process supervision is not running, so nothing can be observed.");
        const auto path = target.path;
        const auto minutes = target.minutes;
        discovery_.start(std::move(target));
        audit_->append(caller.sid, "discovery.started", {{"path", path}, {"minutes", minutes}});
        return discovery_.report();
    });

    add("discovery.status", Access::Owner, [this](Who, Args) { return discovery_.report(); }, true);

    add("discovery.stop", Access::Owner, [this](Who caller, Args) {
        discovery_.stop();
        audit_->append(caller.sid, "discovery.stopped", {});
        return discovery_.report();
    });

    add("discovery.apply", Access::Owner, [this](Who caller, Args args) {
        auto profile = find_profile(require_text(args, "profile_id"));
        const auto report = discovery_.report();
        const auto name = require_text(args, "name");
        if (name.empty() || name.size() > 80)
            throw Failure("bad_request", "Give the application a short name.");

        const core::AppIdentity* root = nullptr;
        core::AppIdentity root_identity;
        for (const auto& item : report["items"]) {
            if (item.value("role", "") != "main") continue;
            root_identity = {item.value("path", ""), item.value("sha256", ""), item.value("publisher", ""),
                             item.value("package_family", "")};
            root = &root_identity;
        }
        if (!root) throw Failure("conflict", "The observation did not see the application start.");

        core::Application app;
        app.id = random_id("app-");
        app.name = name;
        const auto mode = args.value("mode", std::string{"approved"});
        app.mode = mode == "related"         ? core::DependencyMode::Related
                   : mode == "selected_only" ? core::DependencyMode::SelectedOnly
                                             : core::DependencyMode::Approved;
        app.components.push_back(
            component_for(*root, core::ComponentRole::Main, core::Scope::Independent, name));

        std::set<std::string> chosen;
        for (const auto& pick : args.value("items", Json::array()))
            chosen.insert(core::normalize_path(pick.value("path", "")) + "|" + pick.value("sha256", ""));
        const bool acknowledged = args.value("acknowledge_sensitive", false);
        for (const auto& item : report["items"]) {
            const auto key = core::normalize_path(item.value("path", "")) + "|" + item.value("sha256", "");
            const auto verdict = item.value("verdict", "");
            if (!chosen.contains(key) || verdict == "main" || verdict == "windows") continue;
            if (verdict == "avoid" && !acknowledged)
                throw Failure("confirmation_required",
                              "A tool that can run other programs was selected. Confirm to include it.");
            const core::AppIdentity identity{item.value("path", ""), item.value("sha256", ""),
                                             item.value("publisher", ""), item.value("package_family", "")};
            const bool descendant = item.value("descendant", false);
            if (!descendant && identity.sha256.empty() && identity.publisher.empty())
                throw Failure("bad_request",
                              "A component started outside the application needs a hash or signer.");
            app.components.push_back(
                component_for(identity, role_from(item.value("role", "helper")),
                              descendant ? core::Scope::Descendant : core::Scope::Independent, {}));
        }
        const auto root_key = core::normalize_path(root_identity.path);
        auto known = std::ranges::find_if(profile.apps, [&](const core::Application& candidate) {
            return std::ranges::any_of(candidate.components, [&](const core::Component& c) {
                return c.role == core::ComponentRole::Main && core::normalize_path(c.path) == root_key;
            });
        });
        if (known != profile.apps.end()) {
            for (auto& component : app.components) {
                const bool present = std::ranges::any_of(known->components, [&](const core::Component& c) {
                    return core::normalize_path(c.path) == core::normalize_path(component.path);
                });
                if (!present) known->components.push_back(std::move(component));
            }
            known->mode = app.mode;
            app = *known;
        } else {
            profile.apps.push_back(app);
        }
        if (args.value("save_to_library", false)) {
            auto copy = app;
            store_.state.library.push_back(std::move(copy));
        }
        discovery_.stop();
        store_profile(caller.sid, profile, "app.saved");
        return Json{{"app", core::to_json(app)}, {"findings", compile(profile, {}, {}, false).findings}};
    });

    add("reviews.list", Access::Owner, [this](Who, Args) {
        Json list = Json::array();
        std::scoped_lock lock(review_mutex_);
        for (const auto& [id, review] : reviews_) {
            list.push_back({{"id", review.id},
                            {"app_id", review.app_id},
                            {"kind", review.kind},
                            {"path", review.path},
                            {"sha256", review.sha256},
                            {"publisher", review.publisher},
                            {"parent", review.parent_path},
                            {"profile_id", review.profile_id},
                            {"count", review.count},
                            {"first", review.first},
                            {"last", review.last}});
        }
        return list;
    });

    add("reviews.dismiss", Access::Owner, [this](Who, Args args) {
        std::scoped_lock lock(review_mutex_);
        reviews_.erase(require_text(args, "id"));
        return Json::object();
    });

    add("reviews.approve", Access::Owner, [this](Who caller, Args args) {
        Review review;
        {
            std::scoped_lock lock(review_mutex_);
            const auto it = reviews_.find(require_text(args, "id"));
            if (it == reviews_.end()) throw Failure("not_found", "This item is no longer pending.");
            review = it->second;
        }
        auto reply = approve_component(caller.sid, review.profile_id, review.app_id, review.path, args);
        std::scoped_lock lock(review_mutex_);
        reviews_.erase(review.id);
        return reply;
    });

    add("apps.component.add", Access::Owner, [this](Who caller, Args args) {
        return approve_component(caller.sid, require_text(args, "profile_id"), require_text(args, "app_id"),
                                 require_text(args, "path"), args);
    });

    add("blocks.list", Access::Owner, [this](Who, Args args) {
        return blocks(static_cast<std::size_t>(require_int(args, "limit", 1, 500)));
    });

    add("continuity.report", Access::Owner, [this](Who, Args) {
        std::vector<std::string> guests;
        if (store_.state.session) guests = store_.state.session->guest_sids;
        auto report = continuity_.report_json(guests);
        report["cli"] = system::path_to_utf8(install_dir_ / L"houseguest-cli.exe");
        return report;
    });
}

}  // namespace houseguest::service
