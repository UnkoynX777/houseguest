#include <algorithm>
#include <atomic>
#include <thread>
#include <set>

#include "houseguest/system/https.hpp"

#include "cloudflare/process.hpp"
#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

namespace {

// The service-side allowlist. The router maps routes to these names; this second list means a mistake in the
// route table still cannot reach a command that changes credentials, restores state or reaches the system.
const std::set<std::string, std::less<>> kApiCommands = {
    "status",         "dashboard",       "profiles.list", "library.list",    "blocks.list",
    "audit.list",     "diagnostics",     "reviews.list",  "reviews.approve", "reviews.dismiss",
    "profiles.save",  "session.start",   "session.stop",  "session.extend", "events.wait", "users.list"};

}  // namespace

class Controller::ApiBridge final : public api::Host {
public:
    explicit ApiBridge(Controller& controller) : controller_(controller) {}

    Json run(const std::string& command, const Json& args, const std::string& device_id) override {
        return controller_.run_api(command, args, device_id);
    }
    bool check_pin(const std::string& pin) override { return controller_.api_check_pin(pin); }
    void audit(const std::string& actor, const std::string& event, Json detail) override {
        controller_.audit_->append(actor, event, std::move(detail));
    }
    Json health() override { return Json{{"version", kVersion}}; }
    [[nodiscard]] std::vector<std::uint8_t> key() const override {
        return controller_.store_.derive_key("houseguest.api.devices.v1");
    }

private:
    Controller& controller_;
};

void Controller::start_api() {
    api_host_ = std::make_shared<ApiBridge>(*this);
    api_ = std::make_unique<api::ApiService>(*api_host_, store_.directory());
    std::string error;
    if (!api_->start(error)) {
        // The local API is optional: enforcement and the pipe keep working without it.
        audit_->append("service", "api.start_failed", {{"error", error}});
        api_.reset();
        return;
    }
    remote_ = std::make_unique<RemoteAccess>(
        *api_, store_.directory(), [](const system::HttpsRequest& request) { return system::https_request(request); },
        cloudflare::windows_launcher(),
        std::vector<std::filesystem::path>{install_dir_, store_.directory() / L"bin"});
    remote_->resume();
}

Json Controller::sanitize_api_args(const std::string& command, const Json& args) {
    Json scoped = args.is_object() ? args : Json::object();
    if (command == "audit.list" && !scoped.contains("limit")) scoped["limit"] = 50;
    if (command == "blocks.list" && !scoped.contains("limit")) scoped["limit"] = 50;
    if (command == "session.start") {
        // A remote request can start protection but cannot weaken it: no audit-only sessions and no skipped warnings.
        scoped["audit_only"] = false;
        scoped["confirm_warnings"] = false;
    }
    return scoped;
}

Json Controller::run_api(const std::string& command, const Json& args, const std::string& device_id) {
    if (!kApiCommands.contains(command)) throw Failure("forbidden", "This operation is not available remotely.");
    if (command == "events.wait") return wait_for_events(args);
    const auto found = commands_.find(command);
    if (found == commands_.end()) throw Failure("not_found", "Unknown operation.");
    std::string owner;
    {
        std::scoped_lock lock(mutex_);
        if (!store_.state.configured()) throw Failure("not_configured", "Houseguest is not set up yet.");
        owner = store_.state.owner_sid;
    }
    const Caller caller{owner, false};
    const auto& entry = found->second;
    if (entry.access == Access::Elevated) throw Failure("forbidden", "This operation is not available remotely.");
    const Json scoped = sanitize_api_args(command, args);
    (void)device_id;
    if (entry.unlocked) return entry.run(caller, scoped);
    std::scoped_lock lock(mutex_);
    return entry.run(caller, scoped);
}

Json Controller::wait_for_events(const Json& args) {
    static std::atomic<int> waiting{0};
    struct Slot {
        Slot() { ++waiting; }
        ~Slot() { --waiting; }
    };
    if (waiting >= 8) throw Failure("limit", "Too many clients are waiting for events.");
    Slot slot;
    auto number = [&](const char* key) -> std::int64_t {
        return args.contains(key) && args[key].is_number_integer() ? args[key].get<std::int64_t>() : -1;
    };
    const auto policy = number("policy"), history = number("history"), catalog = number("catalog");
    const auto limit = std::chrono::seconds(std::clamp<std::int64_t>(number("wait") < 0 ? 20 : number("wait"), 0, 25));
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        std::uint64_t now_policy, now_catalog, now_history;
        {
            std::scoped_lock lock(mutex_);
            now_policy = store_.revision();
            now_catalog = catalog_.generation();
            now_history = audit_->sequence();
        }
        const bool changed = (policy >= 0 && static_cast<std::int64_t>(now_policy) != policy) ||
                             (history >= 0 && static_cast<std::int64_t>(now_history) != history) ||
                             (catalog >= 0 && static_cast<std::int64_t>(now_catalog) != catalog) ||
                             (policy < 0 && history < 0 && catalog < 0);
        if (changed || api_stopping_ || std::chrono::steady_clock::now() >= deadline) {
            Json result{{"changed", changed},
                        {"generations", {{"policy", now_policy}, {"catalog", now_catalog}, {"history", now_history}}}};
            if (changed && history >= 0 && static_cast<std::int64_t>(now_history) != history)
                result["recent"] = audit_->tail(5);
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

bool Controller::api_check_pin(const std::string& pin) {
    // Remote confirmations have their own failure counter: a stolen device token can neither lock the owner out
    // of the local PIN nor probe it without limit. The local counter is never touched from here.
    std::scoped_lock lock(mutex_);
    if (!store_.state.configured()) return false;
    const auto now = now_seconds();
    if (remote_locked_until_ > now) return false;
    if (pin_matches(pin)) {
        remote_failures_ = 0;
        return true;
    }
    if (++remote_failures_ >= 5)
        remote_locked_until_ = now + std::min<std::int64_t>(3600, 60LL << std::min(remote_failures_ - 5, 5));
    audit_->append("api", "api.pin.failed", {{"failures", remote_failures_}});
    return false;
}

void Controller::register_api_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("api.info", Access::Owner,
        [this](Who, Args) { return api_ ? api_->info() : Json{{"running", false}, {"devices", Json::array()}}; },
        true);

    add("api.devices.create", Access::Owner,
        [this](Who caller, Args args) {
            if (!api_) throw Failure("unavailable", "The local API is not running.");
            const auto name = require_text(args, "name");
            const auto scope = args.value("scope", std::string("read"));
            if (scope != "read" && scope != "write") throw Failure("bad_request", "Scope must be read or write.");
            const auto issued = api_->pair(name, scope == "write" ? api::Scope::Write : api::Scope::Read);
            audit_->append(caller.sid, "api.device.paired", {{"id", issued.device.id}, {"name", name}, {"scope", scope}});
            return Json{{"id", issued.device.id}, {"token", issued.token}, {"scope", scope}, {"expires", issued.device.expires}};
        },
        true);

    add("api.devices.revoke", Access::Owner,
        [this](Who caller, Args args) {
            if (!api_) throw Failure("unavailable", "The local API is not running.");
            const auto id = require_text(args, "id");
            if (!api_->revoke(id)) throw Failure("not_found", "No such active device.");
            audit_->append(caller.sid, "api.device.revoked", {{"id", id}});
            return Json::object();
        },
        true);

    add("api.devices.revoke_all", Access::Owner,
        [this](Who caller, Args) {
            if (api_) api_->revoke_all();
            audit_->append(caller.sid, "api.device.revoked_all", {});
            return Json::object();
        },
        true);
}

}  // namespace houseguest::service

namespace houseguest::service {

void Controller::register_remote_commands() {
    using Args = const Json&;
    using Who = const Caller&;
    auto need = [this] {
        if (!remote_) throw Failure("unavailable", "Remote access is not available.");
        return remote_.get();
    };
    auto text = [](Args args, const char* key) { return args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : std::string(); };

    add("remote.status", Access::Owner, [need](Who, Args) { return need()->status(); }, true);
    add("remote.logs", Access::Owner, [need](Who, Args) { return need()->logs(); }, true);
    add("remote.discover", Access::Owner,
        [need, text](Who, Args args) { return need()->discover(text(args, "api_token")); }, true);
    add("remote.setup", Access::Owner,
        [this, need, text](Who caller, Args args) {
            RemoteAccess::SetupRequest request{text(args, "api_token"), text(args, "account_id"), text(args, "zone_id"),
                                               text(args, "hostname"), {}};
            if (args.contains("emails") && args["emails"].is_array())
                for (const auto& e : args["emails"])
                    if (e.is_string()) request.emails.push_back(e.get<std::string>());
            auto result = need()->setup(request);
            audit_->append(caller.sid, "remote.setup", {{"hostname", result["hostname"]}});
            return result;
        },
        true);
    add("remote.install", Access::Owner,
        [this, need](Who caller, Args) {
            auto result = need()->install_component();
            audit_->append(caller.sid, "remote.component_installed", {});
            return result;
        },
        true);
    add("remote.enable", Access::Owner,
        [this, need](Who caller, Args) {
            auto result = need()->enable();
            audit_->append(caller.sid, "remote.enabled", {});
            return result;
        },
        true);
    add("remote.disable", Access::Owner,
        [this, need](Who caller, Args) {
            auto result = need()->disable();
            audit_->append(caller.sid, "remote.disabled", {});
            return result;
        },
        true);
    add("remote.test", Access::Owner, [need](Who, Args) { return need()->test(); }, true);
    add("remote.reset", Access::Owner,
        [this, need, text](Who caller, Args args) {
            auto result = need()->reset(text(args, "api_token"));
            audit_->append(caller.sid, "remote.reset", {{"removed", result["removed"]}});
            return result;
        },
        true);
}

}  // namespace houseguest::service
