#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "api/devices.hpp"
#include "api/router.hpp"
#include "command_support.hpp"

using namespace houseguest::service;
using namespace houseguest::service::api;
using houseguest::core::Json;

namespace {

struct Harness {
    DeviceRegistry devices;
    std::int64_t now = 1'000'000;
    std::vector<std::string> commands;
    std::vector<std::string> audit;
    std::string pin = "2468";
    std::unique_ptr<Router> router;
    std::string nonce_seed = "n";
    int counter = 0;

    Harness() {
        RouterConfig config;
        config.hosts = {"127.0.0.1:5000", "hg.example.com"};
        config.origins = {"https://hg.example.com"};
        config.devices = &devices;
        config.clock = [this] { return now; };
        config.gateway = [this](const std::string& command, const Json&, const Device&) {
            commands.push_back(command);
            if (command == "boom") throw Failure("forbidden", "no");
            return Json{{"ran", command}};
        };
        config.pin_check = [this](const std::string& p) { return p == pin; };
        config.audit = [this](const std::string&, const std::string& event, Json) { audit.push_back(event); };
        config.health = [] { return Json{{"up", true}}; };
        router = std::make_unique<Router>(std::move(config));
    }

    Request make(const std::string& method, const std::string& path, const std::string& token = {},
                 const std::string& body = {}, bool fresh = true) {
        Request r;
        r.method = method;
        r.target = path;
        r.headers["host"] = "127.0.0.1:5000";
        if (!token.empty()) r.headers["authorization"] = "Bearer " + token;
        if (method != "GET") {
            r.headers["content-type"] = "application/json";
            if (fresh) {
                r.headers["x-hg-timestamp"] = std::to_string(now);
                r.headers["x-hg-nonce"] = "nonce-" + std::to_string(++counter) + "-abcdefghijklmnop";
            }
        }
        r.body = body;
        return r;
    }
};

}  // namespace

TEST_CASE("only a registered device token opens the API", "[api][security]") {
    Harness h;
    const auto read = h.devices.issue("phone", Scope::Read, h.now);
    CHECK(h.router->handle(h.make("GET", "/v1/status")).status == 401);
    CHECK(h.router->handle(h.make("GET", "/v1/status", "hgd_" + std::string(64, 'a'))).status == 401);
    CHECK(h.router->handle(h.make("GET", "/v1/status", "not-a-token")).status == 401);
    const auto ok = h.router->handle(h.make("GET", "/v1/status", read.token));
    CHECK(ok.status == 200);
    CHECK(h.commands == std::vector<std::string>{"status"});
}

TEST_CASE("the token is never stored, only its hash", "[api][security]") {
    DeviceRegistry devices;
    const auto issued = devices.issue("phone", Scope::Read, 100);
    CHECK(devices.to_json().dump().find(issued.token) == std::string::npos);
    CHECK(issued.token.size() == 68);
}

TEST_CASE("health is public and reveals nothing sensitive", "[api]") {
    Harness h;
    const auto r = h.router->handle(h.make("GET", "/v1/health"));
    CHECK(r.status == 200);
    CHECK(h.commands.empty());
}

TEST_CASE("unexpected Host and Origin headers are refused", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("phone", Scope::Read, h.now);
    auto rebinding = h.make("GET", "/v1/status", device.token);
    rebinding.headers["host"] = "evil.example:5000";
    CHECK(h.router->handle(rebinding).status == 403);

    auto foreign = h.make("GET", "/v1/status", device.token);
    foreign.headers["origin"] = "https://evil.example";
    CHECK(h.router->handle(foreign).status == 403);

    auto allowed = h.make("GET", "/v1/status", device.token);
    allowed.headers["origin"] = "https://hg.example.com";
    const auto r = h.router->handle(allowed);
    CHECK(r.status == 200);
    CHECK(r.headers.at("Access-Control-Allow-Origin") == "https://hg.example.com");

    auto preflight = h.make("OPTIONS", "/v1/profiles");
    preflight.headers["origin"] = "https://evil.example";
    CHECK(h.router->handle(preflight).status == 403);
    preflight.headers["origin"] = "https://hg.example.com";
    CHECK(h.router->handle(preflight).status == 204);
}

TEST_CASE("a read-only device cannot change anything", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("phone", Scope::Read, h.now);
    const auto r = h.router->handle(h.make("POST", "/v1/session/stop", device.token, "{}"));
    CHECK(r.status == 403);
    CHECK(h.commands.empty());
}

TEST_CASE("operations that are not routed do not exist", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);
    for (const char* path : {"/v1/pin.change", "/v1/settings", "/v1/backup/restore", "/v1/recover", "/v1/unlock",
                              "/v1/accounts", "/v1/exec", "/v1/files", "/"}) {
        CHECK(h.router->handle(h.make("POST", path, device.token, "{}")).status == 404);
        CHECK(h.router->handle(h.make("GET", path, device.token)).status == 404);
    }
    CHECK(h.commands.empty());
    CHECK(h.router->handle(h.make("GET", "/v1/session/stop", device.token)).status == 405);
}

TEST_CASE("mutating requests need a fresh timestamp and a nonce used once", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);

    auto stale = h.make("POST", "/v1/session/stop", device.token, "{}");
    stale.headers["x-hg-timestamp"] = std::to_string(h.now - 3600);
    CHECK(h.router->handle(stale).status == 400);

    CHECK(h.router->handle(h.make("POST", "/v1/session/stop", device.token, "{}", false)).status == 400);

    auto first = h.make("POST", "/v1/session/stop", device.token, "{}");
    CHECK(h.router->handle(first).status == 200);
    const auto replay = h.router->handle(first);  // identical bytes captured on the wire
    CHECK(replay.status == 409);
    CHECK(h.commands.size() == 1);
}

TEST_CASE("sensitive changes need a PIN step-up that expires", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);

    CHECK(h.router->handle(h.make("POST", "/v1/profiles", device.token, "{}")).status == 403);
    CHECK(h.router->handle(h.make("POST", "/v1/stepup", device.token, R"({"pin":"0000"})")).status == 401);
    CHECK(h.router->handle(h.make("POST", "/v1/stepup", device.token, R"({"pin":"2468"})")).status == 200);
    CHECK(h.router->handle(h.make("POST", "/v1/profiles", device.token, "{}")).status == 200);

    h.now += Router::kStepupSeconds + 1;
    CHECK(h.router->handle(h.make("POST", "/v1/profiles", device.token, "{}")).status == 403);
}

TEST_CASE("a read device cannot step up", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("phone", Scope::Read, h.now);
    CHECK(h.router->handle(h.make("POST", "/v1/stepup", device.token, R"({"pin":"2468"})")).status == 403);
}

TEST_CASE("repeated wrong PINs lock the step-up even for the right PIN", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);
    for (int i = 0; i < 5; ++i)
        CHECK(h.router->handle(h.make("POST", "/v1/stepup", device.token, R"({"pin":"1111"})")).status == 401);
    CHECK(h.router->handle(h.make("POST", "/v1/stepup", device.token, R"({"pin":"2468"})")).status == 429);
    h.now += 120;
    CHECK(h.router->handle(h.make("POST", "/v1/stepup", device.token, R"({"pin":"2468"})")).status == 200);
}

TEST_CASE("revoked, expired and idle devices are refused", "[api][security]") {
    Harness h;
    const auto a = h.devices.issue("a", Scope::Read, h.now);
    const auto b = h.devices.issue("b", Scope::Read, h.now);
    const auto c = h.devices.issue("c", Scope::Read, h.now);
    CHECK(h.router->handle(h.make("GET", "/v1/status", a.token)).status == 200);

    h.devices.revoke(a.device.id);
    CHECK(h.router->handle(h.make("GET", "/v1/status", a.token)).status == 401);

    h.now += kDeviceIdle + 1;  // b and c were idle
    CHECK(h.router->handle(h.make("GET", "/v1/status", b.token)).status == 401);

    h.devices.load(h.devices.to_json());
    const auto fresh = h.devices.issue("d", Scope::Read, h.now);
    h.now += kDeviceLifetime + 1;
    CHECK(h.router->handle(h.make("GET", "/v1/status", fresh.token)).status == 401);
    (void)c;
}

TEST_CASE("a device can sign itself out and list devices", "[api]") {
    Harness h;
    const auto device = h.devices.issue("phone", Scope::Read, h.now);
    const auto listing = h.router->handle(h.make("GET", "/v1/devices", device.token));
    REQUIRE(listing.status == 200);
    CHECK(Json::parse(listing.body)["data"][0]["current"] == true);
    CHECK(listing.body.find("token_hash") == std::string::npos);
    CHECK(h.router->handle(h.make("POST", "/v1/devices/self/revoke", device.token, "{}")).status == 200);
    CHECK(h.router->handle(h.make("GET", "/v1/status", device.token)).status == 401);
}

TEST_CASE("the pairing limit is enforced", "[api]") {
    DeviceRegistry devices;
    for (std::size_t i = 0; i < kMaxDevices; ++i) devices.issue("d" + std::to_string(i), Scope::Read, 10);
    CHECK_THROWS(devices.issue("one too many", Scope::Read, 10));
}

TEST_CASE("the identity precheck can refuse before authentication", "[api][security]") {
    Harness h;
    RouterConfig config;
    config.hosts = {"127.0.0.1:5000"};
    config.devices = &h.devices;
    config.clock = [&] { return h.now; };
    config.gateway = [](const std::string&, const Json&, const Device&) { return Json::object(); };
    config.precheck = [](const Request&) { return std::optional<std::string>("Cloudflare Access identity required."); };
    Router router(std::move(config));
    const auto device = h.devices.issue("phone", Scope::Read, h.now);
    CHECK(router.handle(h.make("GET", "/v1/status", device.token)).status == 403);
}

TEST_CASE("bad bodies are rejected before reaching the gateway", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);
    CHECK(h.router->handle(h.make("POST", "/v1/session/stop", device.token, "[1]")).status == 400);
    CHECK(h.router->handle(h.make("POST", "/v1/session/stop", device.token, "{nope")).status == 400);
    CHECK(h.router->handle(h.make("POST", "/v1/session/stop", device.token, std::string(100, '['))).status == 400);
    auto wrong_type = h.make("POST", "/v1/session/stop", device.token, "{}");
    wrong_type.headers["content-type"] = "text/plain";
    CHECK(h.router->handle(wrong_type).status == 415);
    CHECK(h.commands.empty());
}

TEST_CASE("changes made by a device are audited", "[api]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);
    CHECK(h.router->handle(h.make("POST", "/v1/session/stop", device.token, "{}")).status == 200);
    REQUIRE_FALSE(h.audit.empty());
    CHECK(h.audit.back() == "api.session.stop");
}

TEST_CASE("a page may call the API that served it, and no other site may", "[api][security]") {
    Harness h;
    const auto device = h.devices.issue("tablet", Scope::Write, h.now);
    auto own = h.make("POST", "/v1/session/stop", device.token, "{}");
    own.headers["origin"] = "http://127.0.0.1:5000";
    CHECK(h.router->handle(own).status == 200);
    auto public_page = h.make("POST", "/v1/session/stop", device.token, "{}");
    public_page.headers["host"] = "hg.example.com";
    public_page.headers["origin"] = "https://hg.example.com";
    CHECK(h.router->handle(public_page).status == 200);
    auto other_port = h.make("POST", "/v1/session/stop", device.token, "{}");
    other_port.headers["origin"] = "http://127.0.0.1:6000";
    CHECK(h.router->handle(other_port).status == 403);
    auto scheme_swap = h.make("POST", "/v1/session/stop", device.token, "{}");
    scheme_swap.headers["host"] = "hg.example.com";
    scheme_swap.headers["origin"] = "https://hg.example.com.evil.net";
    CHECK(h.router->handle(scheme_swap).status == 403);
}
