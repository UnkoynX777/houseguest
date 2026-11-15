#include <set>

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
    "profiles.save",  "session.start",   "session.stop",  "session.extend"};

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
    }
}

Json Controller::run_api(const std::string& command, const Json& args, const std::string& device_id) {
    if (!kApiCommands.contains(command)) throw Failure("forbidden", "This operation is not available remotely.");
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
    const Json scoped = args.is_object() ? args : Json::object();
    (void)device_id;
    if (entry.unlocked) return entry.run(caller, scoped);
    std::scoped_lock lock(mutex_);
    return entry.run(caller, scoped);
}

bool Controller::api_check_pin(const std::string& pin) {
    std::scoped_lock lock(mutex_);
    if (!store_.state.configured()) return false;
    try {
        verify_pin(pin);
        return true;
    } catch (const Failure&) {
        return false;
    }
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
