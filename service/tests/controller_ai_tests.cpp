#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "command_support.hpp"
#include "controller.hpp"

using namespace houseguest::service;
using houseguest::core::Json;

namespace {

struct Fixture {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "hg-controller-ai";
    std::unique_ptr<Controller> controller;
    std::string token;
    Caller owner{"S-1-5-21-1-2-3-1001", true};
    Fixture() {
        std::filesystem::remove_all(dir);
        controller = std::make_unique<Controller>(dir, false);
        const auto reply = controller->handle(owner, {{"cmd", "setup"}, {"args", {{"pin", "2468"}}}});
        token = reply["data"]["token"];
    }
    ~Fixture() {
        controller.reset();
        std::filesystem::remove_all(dir);
    }
    Json call(const std::string& cmd, Json args = Json::object(), bool with_token = true) {
        Json request{{"cmd", cmd}, {"args", std::move(args)}};
        if (with_token) request["token"] = token;
        return controller->handle(owner, request);
    }
};

}  // namespace

TEST_CASE("assistant and remote commands need the owner's unlocked session", "[controller][security]") {
    Fixture f;
    for (const char* cmd : {"ai.status", "ai.key.set", "ai.settings.set", "ai.chat.send", "ai.proposal.apply", "api.info",
                            "api.devices.create", "remote.status", "remote.enable", "remote.setup"}) {
        const auto reply = f.call(cmd, Json::object(), false);
        CHECK(reply["ok"] == false);
        CHECK(reply["error"]["code"] == "unauthorized");
    }
}

TEST_CASE("the assistant is off by default and cannot be used before it is configured", "[controller][assistant]") {
    Fixture f;
    const auto status = f.call("ai.status");
    REQUIRE(status["ok"] == true);
    CHECK(status["data"]["enabled"] == false);
    CHECK(status["data"]["has_key"] == false);
    CHECK(status["data"]["privacy"].get<std::string>().find("Google") != std::string::npos);
    CHECK(f.call("ai.chat.send", {{"text", "hello"}})["error"]["code"] == "disabled");
    CHECK(f.call("ai.settings.set", {{"enabled", true}})["error"]["code"] == "no_key");
    CHECK(f.call("ai.key.set", {{"api_key", "nope"}})["error"]["code"] == "bad_request");
    CHECK(f.call("ai.proposals")["data"].empty());
    CHECK(f.call("ai.proposal.apply", {{"id", "prop-x"}})["error"]["code"] == "not_found");
}

TEST_CASE("remote access is unavailable and inert without the local API", "[controller][remote]") {
    Fixture f;
    CHECK(f.call("remote.status")["error"]["code"] == "unavailable");
    CHECK(f.call("api.info")["data"]["running"] == false);
    CHECK(f.call("api.devices.create", {{"name", "phone"}})["error"]["code"] == "unavailable");
}

TEST_CASE("the unlocked local session cannot reach assistant commands remotely", "[controller][security]") {
    Fixture f;
    for (const char* cmd : {"ai.chat.send", "ai.proposal.apply", "ai.key.set", "remote.enable", "api.devices.create",
                            "settings.set", "pin.change", "backup.restore", "recover", "unlock"}) {
        try {
            (void)f.controller->run_api(cmd, Json::object(), "test");
            FAIL(std::string(cmd) + " should not be reachable from the API");
        } catch (const Failure& e) {
            CHECK(e.code == "forbidden");
        }
    }
    CHECK_NOTHROW((void)f.controller->run_api("status", Json::object(), "test"));
}

TEST_CASE("a remote session cannot be started in audit mode or past warnings", "[controller][security]") {
    Fixture f;
    const auto args = Controller::sanitize_api_args("session.start", {{"audit_only", true}, {"confirm_warnings", true}});
    CHECK(args["audit_only"] == false);
    CHECK(args["confirm_warnings"] == false);
}

TEST_CASE("remote PIN failures do not touch the local PIN lock", "[controller][security]") {
    Fixture f;
    for (int i = 0; i < 8; ++i) CHECK_FALSE(f.controller->api_check_pin("0000"));
    CHECK(f.controller->api_check_pin("2468") == false);  // the remote lock holds even for the right PIN
    const auto unlock = f.controller->handle(f.owner, {{"cmd", "unlock"}, {"args", {{"pin", "2468"}}}});
    CHECK(unlock["ok"] == true);  // the owner is not locked out locally
}
