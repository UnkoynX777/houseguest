#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "command_support.hpp"
#include "remote.hpp"

using namespace houseguest::service;
namespace sys = houseguest::system;
using houseguest::core::Json;

namespace {

struct FakeHost final : api::Host {
    Json run(const std::string&, const Json&, const std::string&) override { return Json::object(); }
    bool check_pin(const std::string&) override { return false; }
    void audit(const std::string&, const std::string&, Json) override {}
    Json health() override { return Json::object(); }
    [[nodiscard]] std::vector<std::uint8_t> key() const override { return std::vector<std::uint8_t>(32, 3); }
};

sys::HttpsResponse ok(const Json& result) {
    return {200, Json{{"success", true}, {"result", result}}.dump(), {}};
}

struct Fixture {
    FakeHost host;
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "hg-remote";
    std::unique_ptr<api::ApiService> api;
    std::vector<std::string> calls;
    std::vector<sys::HttpsResponse> script;
    std::unique_ptr<RemoteAccess> remote;

    Fixture() {
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        api = std::make_unique<api::ApiService>(host, dir);
        remote = std::make_unique<RemoteAccess>(
            *api, dir,
            [this](const sys::HttpsRequest& r) {
                calls.push_back(
                    r.method + " " +
                    r.url.substr(r.url.find("/v4") == std::string::npos ? 0 : r.url.find("/v4") + 3));
                if (script.empty()) return ok(Json::object());
                auto reply = script.front();
                script.erase(script.begin());
                return reply;
            },
            [](const cloudflare::LaunchSpec&, cloudflare::LineSink)
                -> std::unique_ptr<cloudflare::RunningProcess> { throw std::runtime_error("not expected"); },
            std::vector<std::filesystem::path>{dir / "bin"});
    }
    ~Fixture() {
        remote.reset();
        api.reset();
        std::filesystem::remove_all(dir);
    }
    RemoteAccess::SetupRequest request() {
        return {std::string(40, 'x'),
                "0123456789abcdef0123456789abcdef",
                "fedcba9876543210fedcba9876543210",
                "hg.example.com",
                {"owner@example.com"}};
    }
    void happy_script() {
        script = {ok(Json::object()),
                  ok({{"auth_domain", "team.cloudflareaccess.com"}}),
                  ok({{"id", "11111111-2222-3333-4444-555555555555"}}),
                  ok(Json::object()),
                  ok({{"id", "pol"}}),
                  ok({{"id", "app"}, {"aud", "aud-tag"}})};
    }
};

}  // namespace

TEST_CASE("setup creates the tunnel, DNS record and Access policy in order", "[remote][integration]") {
    Fixture f;
    f.happy_script();
    const auto result = f.remote->setup(f.request());
    CHECK(result["hostname"] == "hg.example.com");
    REQUIRE(f.calls.size() == 6);
    CHECK(f.calls[0].starts_with("GET /user/tokens/verify"));
    CHECK(f.calls[2].starts_with("POST /accounts/"));
    CHECK(f.calls[3].find("/dns_records") != std::string::npos);
    CHECK(f.calls[4].find("/access/policies") != std::string::npos);
    CHECK(f.calls[5].find("/access/apps") != std::string::npos);
    const auto status = f.remote->status();
    CHECK(status["configured"] == true);
    CHECK(status["enabled"] == false);
    CHECK(status["access_policy"] == true);
    CHECK(status.dump().find("secret") == std::string::npos);
}

TEST_CASE("a failed setup removes what it created", "[remote][integration]") {
    Fixture f;
    f.script = {ok(Json::object()),
                ok({{"auth_domain", "team.cloudflareaccess.com"}}),
                ok({{"id", "11111111-2222-3333-4444-555555555555"}}),
                ok(Json::object()),
                ok({{"id", "pol"}}),
                {403, R"({"success":false,"errors":[{"message":"Access is not enabled"}]})", {}}};
    CHECK_THROWS_AS(f.remote->setup(f.request()), Failure);
    CHECK(f.remote->status()["configured"] == false);
    const auto undone = std::count_if(f.calls.begin(), f.calls.end(),
                                      [](const std::string& c) { return c.starts_with("DELETE"); });
    CHECK(undone >= 2);  // the DNS record and the tunnel
}

TEST_CASE("setup refuses bad input before talking to Cloudflare", "[remote][security]") {
    Fixture f;
    auto bad = f.request();
    bad.hostname = "bad host";
    CHECK_THROWS_AS(f.remote->setup(bad), Failure);
    bad = f.request();
    bad.emails.clear();
    CHECK_THROWS_AS(f.remote->setup(bad), Failure);
    bad = f.request();
    bad.emails = {"not-an-email"};
    CHECK_THROWS_AS(f.remote->setup(bad), Failure);
    bad = f.request();
    bad.account_id = "../x";
    CHECK_THROWS_AS(f.remote->setup(bad), Failure);
    bad = f.request();
    bad.api_token = "short";
    CHECK_THROWS_AS(f.remote->setup(bad), Failure);
    CHECK(f.calls.empty());
}

TEST_CASE("remote access cannot be enabled before setup, a policy or the component", "[remote][security]") {
    Fixture f;
    CHECK_THROWS_AS(f.remote->enable(), Failure);  // not configured
    f.happy_script();
    f.remote->setup(f.request());
    CHECK_THROWS_AS(f.remote->enable(), Failure);  // API not running
    std::string error;
    REQUIRE(f.api->start(error));
    try {
        f.remote->enable();
        FAIL("enable should need the component");
    } catch (const Failure& e) {
        CHECK(e.code == "install_required");
    }
    CHECK(f.remote->status()["enabled"] == false);
    CHECK_FALSE(f.api->access().config().enabled);
}

TEST_CASE("the secret is stored protected and the configuration survives a restart", "[remote][security]") {
    Fixture f;
    f.happy_script();
    f.remote->setup(f.request());
    std::string text;
    {
        std::ifstream in(f.dir / "remote.json", std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    REQUIRE_FALSE(text.empty());
    const auto json = Json::parse(text);
    CHECK(json["secret"].get<std::string>().size() > 100);  // DPAPI blob, not the 44-character base64 secret
    CHECK(json["secret"].get<std::string>().find('=') == std::string::npos);

    RemoteAccess again(
        *f.api, f.dir, [](const sys::HttpsRequest&) { return sys::HttpsResponse{}; },
        [](const cloudflare::LaunchSpec&,
           cloudflare::LineSink) -> std::unique_ptr<cloudflare::RunningProcess> { return nullptr; },
        {f.dir / "bin"});
    CHECK(again.status()["configured"] == true);
}

TEST_CASE("a damaged configuration is forgotten, not run", "[remote][security]") {
    Fixture f;
    {
        std::ofstream out(f.dir / "remote.json", std::ios::binary | std::ios::trunc);
        out << R"({"configured":true,"enabled":true,"hostname":"evil host","tunnel_id":"x","secret":"00"})";
    }
    RemoteAccess again(
        *f.api, f.dir, [](const sys::HttpsRequest&) { return sys::HttpsResponse{}; },
        [](const cloudflare::LaunchSpec&,
           cloudflare::LineSink) -> std::unique_ptr<cloudflare::RunningProcess> { return nullptr; },
        {f.dir / "bin"});
    CHECK(again.status()["configured"] == false);
}

TEST_CASE("the exposure test fails closed when the API answers anonymous visitors", "[remote][security]") {
    Fixture f;
    f.happy_script();
    f.remote->setup(f.request());
    f.script = {{200, R"({"ok":true,"data":{}})", {}}};
    CHECK(f.remote->test()["protected"] == false);
    f.script = {{200, "<html>Cloudflare Access sign in</html>", {}}};
    CHECK(f.remote->test()["protected"] == true);
    f.script = {{403, "", {}}};
    CHECK(f.remote->test()["protected"] == true);
    f.script = {{0, "", "unreachable"}};
    CHECK(f.remote->test()["reachable"] == false);
}

TEST_CASE("reset removes the cloud resources and forgets the configuration", "[remote][integration]") {
    Fixture f;
    f.happy_script();
    f.remote->setup(f.request());
    f.calls.clear();
    f.script = {ok(Json::object()), ok(Json::array()), ok(Json::object()), ok(Json::object())};
    const auto result = f.remote->reset(std::string(40, 'x'));
    CHECK(result["removed"].size() == 3);
    CHECK(f.remote->status()["configured"] == false);
}
