#include <atomic>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "cloudflare/api.hpp"
#include "cloudflare/tunnel.hpp"

using namespace houseguest::service::cloudflare;
using namespace std::chrono_literals;
using houseguest::core::Json;
namespace sys = houseguest::system;

namespace {

struct Recorder {
    std::vector<sys::HttpsRequest> requests;
    std::vector<sys::HttpsResponse> replies;
    Transport transport() {
        return [this](const sys::HttpsRequest& r) {
            requests.push_back(r);
            if (replies.empty()) return sys::HttpsResponse{200, R"({"success":true,"result":{}})", {}};
            auto reply = replies.front();
            replies.erase(replies.begin());
            return reply;
        };
    }
};

sys::HttpsResponse ok(const Json& result) { return {200, Json{{"success", true}, {"result", result}}.dump(), {}}; }

}  // namespace

TEST_CASE("hostnames are validated before they reach DNS, YAML or URLs", "[cloudflare][security]") {
    CHECK(valid_hostname("hg.example.com"));
    CHECK(valid_hostname("a-b.c.example.co.uk"));
    for (const char* bad : {"", "localhost", "-a.example.com", "a..example.com", "a.example.com\nservice: x",
                            "a.example.com:8080", "UPPER.example.com", "a b.example.com", "a.example.com/x", "*.example.com"})
        CHECK_FALSE(valid_hostname(bad));
}

TEST_CASE("the tunnel is created with a local secret and the token is only sent as a bearer header", "[cloudflare]") {
    Recorder rec;
    rec.replies = {ok({{"id", "11111111-2222-3333-4444-555555555555"}})};
    Api api(rec.transport(), "secret-api-token");
    const auto created = api.create_tunnel("acct", "houseguest");
    CHECK(created.tunnel_id == "11111111-2222-3333-4444-555555555555");
    CHECK(created.secret_base64.size() == 44);
    REQUIRE(rec.requests.size() == 1);
    CHECK(rec.requests[0].method == "POST");
    CHECK(rec.requests[0].url.starts_with("https://api.cloudflare.com/client/v4/"));
    CHECK(rec.requests[0].body.find("secret-api-token") == std::string::npos);
    CHECK(Json::parse(rec.requests[0].body)["config_src"] == "local");
}

TEST_CASE("Access policy allows only the named e-mail addresses and returns the audience", "[cloudflare][security]") {
    Recorder rec;
    rec.replies = {ok({{"id", "policy1"}}), ok({{"id", "app1"}, {"aud", "audience-tag"}})};
    Api api(rec.transport(), "t");
    const auto app = api.create_access_app("acct", "Houseguest", "hg.example.com", {"owner@example.com"});
    CHECK(app.audience == "audience-tag");
    REQUIRE(rec.requests.size() == 2);
    const auto policy = Json::parse(rec.requests[0].body);
    CHECK(policy["decision"] == "allow");
    CHECK(policy["include"].size() == 1);
    const auto body = Json::parse(rec.requests[1].body);
    CHECK(body["domain"] == "hg.example.com");
    CHECK(body["type"] == "self_hosted");
}

TEST_CASE("an Access application cannot be created without an allowed identity", "[cloudflare][security]") {
    Recorder rec;
    Api api(rec.transport(), "t");
    CHECK_THROWS_AS(api.create_access_app("acct", "Houseguest", "hg.example.com", {}), ApiError);
    CHECK_THROWS_AS(api.create_access_app("acct", "Houseguest", "bad host", {"a@b.c"}), ApiError);
    CHECK(rec.requests.empty());
}

TEST_CASE("API failures carry Cloudflare's message but never the token", "[cloudflare][security]") {
    Recorder rec;
    rec.replies = {{403, R"({"success":false,"errors":[{"code":10000,"message":"Authentication error"}]})", {}},
                   {0, "", "The request could not be sent."},
                   {401, "not json", {}}};
    Api api(rec.transport(), "SECRET-TOKEN-VALUE");
    for (int i = 0; i < 3; ++i) {
        try {
            (void)api.zones();
            FAIL("should have thrown");
        } catch (const ApiError& e) {
            CHECK(std::string(e.what()).find("SECRET-TOKEN-VALUE") == std::string::npos);
        }
    }
}

TEST_CASE("the generated configuration routes one hostname to the local API and nothing else", "[cloudflare]") {
    TunnelSettings s{"11111111-2222-3333-4444-555555555555", "acct", "c2VjcmV0", "hg.example.com"};
    const auto yaml = render_config(s, 49152, "C:/ProgramData/Houseguest/remote/credentials.json");
    CHECK(yaml.find("service: http://127.0.0.1:49152") != std::string::npos);
    CHECK(yaml.find("hostname: hg.example.com") != std::string::npos);
    CHECK(yaml.find("http_status:404") != std::string::npos);
    CHECK(yaml.find("c2VjcmV0") == std::string::npos);  // the secret lives only in the credentials file

    s.hostname = "x.example.com\n  - service: http://evil";
    CHECK_THROWS(render_config(s, 1, "c.json"));
    s.hostname = "hg.example.com";
    s.tunnel_id = "id\nbad";
    CHECK_THROWS(render_config(s, 1, "c.json"));
    CHECK_THROWS(render_config({"11111111-2222", "a", "b", "hg.example.com"}, 1, "has\"quote"));
}

TEST_CASE("log lines are classified and long secrets are hidden", "[cloudflare][security]") {
    CHECK(classify_log_line("INF Registered tunnel connection connIndex=0").kind == LogKind::Connected);
    CHECK(classify_log_line("WRN Unregistered tunnel connection").kind == LogKind::Disconnected);
    CHECK(classify_log_line("ERR failed to dial edge").kind == LogKind::Error);
    CHECK(classify_log_line("INF Starting tunnel").kind == LogKind::Info);
    const auto event = classify_log_line("INF token=" + std::string(60, 'A') + " used");
    CHECK(event.text.find(std::string(60, 'A')) == std::string::npos);
}

TEST_CASE("restarts back off, reset after a stable run and give up when they loop", "[cloudflare]") {
    using namespace std::chrono;
    Backoff backoff;
    auto t = Backoff::Clock::now();
    std::vector<long long> delays;
    for (int i = 0; i < 6; ++i) {
        const auto d = backoff.on_exit(t, t + 1s);
        REQUIRE_FALSE(d.give_up);
        delays.push_back(d.delay.count());
        t += 2s;
    }
    CHECK(delays == std::vector<long long>{1, 2, 4, 8, 16, 32});
    CHECK(backoff.on_exit(t, t + 1s).give_up);

    Backoff stable;
    auto s = Backoff::Clock::now();
    (void)stable.on_exit(s, s + 1s);
    (void)stable.on_exit(s, s + 2s);
    CHECK(stable.on_exit(s, s + Backoff::kStable + 5s).delay == 1s);
}

namespace {

struct FakeProcess final : RunningProcess {
    std::shared_ptr<std::atomic<bool>> running = std::make_shared<std::atomic<bool>>(true);
    bool alive() override { return *running; }
    void stop() override { *running = false; }
    void wait(std::chrono::milliseconds limit) override { std::this_thread::sleep_for(std::min<std::chrono::milliseconds>(limit, 5ms)); }
};

bool wait_for(const std::function<bool()>& condition) {
    for (int i = 0; i < 400; ++i) {
        if (condition()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

}  // namespace

TEST_CASE("the manager connects, reports status, and removes its files on stop", "[cloudflare][integration]") {
    const auto dir = std::filesystem::temp_directory_path() / "hg-tunnel-a";
    std::filesystem::remove_all(dir);
    std::vector<std::shared_ptr<std::atomic<bool>>> started;
    LineSink sink;
    TunnelManager manager([&](const LaunchSpec&, LineSink s) {
        auto process = std::make_unique<FakeProcess>();
        started.push_back(process->running);
        sink = std::move(s);
        return process;
    }, dir);
    TunnelSettings settings{"11111111-2222-3333-4444-555555555555", "acct", "c2VjcmV0", "hg.example.com"};
    std::string error;
    REQUIRE(manager.start("cloudflared.exe", settings, 5555, error));
    CHECK(std::filesystem::exists(dir / "config.yml"));
    CHECK(std::filesystem::exists(dir / "credentials.json"));
    REQUIRE(wait_for([&] { return sink != nullptr; }));
    sink("INF Registered tunnel connection connIndex=0");
    REQUIRE(wait_for([&] { return manager.state() == TunnelState::Connected; }));
    CHECK(manager.status()["hostname"] == "hg.example.com");
    CHECK_FALSE(manager.start("cloudflared.exe", settings, 5555, error));  // one instance only
    manager.stop();
    CHECK(manager.state() == TunnelState::Stopped);
    CHECK_FALSE(std::filesystem::exists(dir / "credentials.json"));
    CHECK_FALSE(*started[0]);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a crashing process is restarted with backoff, then the manager gives up", "[cloudflare][integration]") {
    const auto dir = std::filesystem::temp_directory_path() / "hg-tunnel-b";
    std::filesystem::remove_all(dir);
    std::atomic<int> launches = 0;
    TunnelManager manager([&](const LaunchSpec&, LineSink) {
        ++launches;
        auto process = std::make_unique<FakeProcess>();
        *process->running = false;  // exits at once
        return process;
    }, dir);
    manager.set_delay_unit(1ms);
    TunnelSettings settings{"11111111-2222-3333-4444-555555555555", "acct", "c2VjcmV0", "hg.example.com"};
    std::string error;
    REQUIRE(manager.start("cloudflared.exe", settings, 5555, error));
    REQUIRE(wait_for([&] { return manager.state() == TunnelState::Failed; }));
    CHECK(launches == Backoff::kMaxRestartsPerWindow + 1);
    CHECK(manager.status()["last_error"].get<std::string>().find("kept stopping") != std::string::npos);
    manager.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("a launcher that throws is reported and does not spin", "[cloudflare][integration]") {
    const auto dir = std::filesystem::temp_directory_path() / "hg-tunnel-c";
    std::filesystem::remove_all(dir);
    std::atomic<int> launches = 0;
    TunnelManager manager([&](const LaunchSpec&, LineSink) -> std::unique_ptr<RunningProcess> {
        ++launches;
        throw std::runtime_error("missing");
    }, dir);
    manager.set_delay_unit(1ms);
    TunnelSettings settings{"11111111-2222-3333-4444-555555555555", "acct", "c2VjcmV0", "hg.example.com"};
    std::string error;
    REQUIRE(manager.start("cloudflared.exe", settings, 5555, error));
    REQUIRE(wait_for([&] { return manager.state() == TunnelState::Failed; }));
    CHECK(launches <= Backoff::kMaxRestartsPerWindow + 1);
    CHECK_FALSE(manager.logs().empty());
    manager.stop();
    std::filesystem::remove_all(dir);
}
