// Test-only server: the real API listener, router and remote panel with a stand-in for the service, so the
// panel can be driven in a browser. It prints the port and a read key and a write key, then serves until
// stdin closes.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "api/service.hpp"
#include "command_support.hpp"

using namespace houseguest::service;
using houseguest::core::Json;

namespace {

struct DemoHost final : api::Host {
    std::string state = "active";
    std::int64_t ends = now_seconds() + 2700;
    std::vector<std::string> changes;
    Json run(const std::string& command, const Json& args, const std::string&) override {
        const auto now = now_seconds();
        if (command == "dashboard")
            return Json{{"armed", state == "active"},
                        {"enforcement", "supervised"},
                        {"blocked", 7},
                        {"now", now},
                        {"session", state == "active" ? Json{{"profile_name", "Gaming"}, {"ends_at", ends}}
                                                      : Json(nullptr)}};
        if (command == "status")
            return Json{{"armed", state == "active"},
                        {"now", now},
                        {"generations", {{"policy", 1}, {"catalog", 1}, {"history", 1}}}};
        if (command == "profiles.list")
            return Json::array({{{"id", "p1"},
                                 {"name", "Gaming"},
                                 {"default", "deny"},
                                 {"apps", Json::array({1, 2})},
                                 {"rules", Json::array()}},
                                {{"id", "p2"},
                                 {"name", "Homework"},
                                 {"default", "deny"},
                                 {"apps", Json::array({1})},
                                 {"rules", Json::array({1, 2, 3})}}});
        if (command == "library.list")
            return Json::array({{{"name", "Steam"}, {"components", Json::array({1, 2})}},
                                {{"name", "Discord"}, {"components", Json::array({1})}}});
        if (command == "blocks.list")
            return Json::array(
                {{{"event", "app.blocked"}, {"path", "C:\\Games\\telemetry.exe"}, {"ts", now - 120}},
                 {{"event", "app.would_block"},
                  {"path", "C:\\Tools\\<img src=x onerror=alert(1)>.exe"},
                  {"ts", now - 600}}});
        if (command == "audit.list")
            return Json::array({{{"event", "session.started"}, {"ts", now - 900}},
                                {{"event", "unlock"}, {"ts", now - 1500}}});
        if (command == "reviews.list")
            return Json::array({{{"id", "r1"},
                                 {"kind", "unknown_dependency"},
                                 {"path", "C:\\Games\\Minecraft\\updater.exe"},
                                 {"count", 3}}});
        if (command == "events.wait") {
            for (int i = 0; i < 20 && !changed; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const bool was = changed.exchange(false);
            return Json{{"changed", was},
                        {"generations", {{"policy", 1}, {"catalog", 1}, {"history", ++seq}}}};
        }
        if (command == "session.stop") state = "stopped";
        if (command == "session.extend") ends += args.value("minutes", 0) * 60;
        if (command == "users.list") return Json::array();
        changes.push_back(command);
        changed = true;
        return Json::object();
    }
    bool check_pin(const std::string& pin) override { return pin == "2468"; }
    void audit(const std::string& actor, const std::string& event, Json) override {
        std::cerr << "audit " << actor << " " << event << "\n";
    }
    Json health() override { return Json{{"version", "demo"}}; }
    [[nodiscard]] std::vector<std::uint8_t> key() const override { return std::vector<std::uint8_t>(32, 9); }
    std::atomic<bool> changed = false;
    std::atomic<int> seq = 1;
};

}  // namespace

int main() {
    DemoHost host;
    const auto dir = std::filesystem::temp_directory_path() / "hg-api-demo";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    api::ApiService service(host, dir);
    std::string error;
    if (!service.start(error)) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto read = service.pair("Tablet", api::Scope::Read);
    const auto write = service.pair("Pixel 8", api::Scope::Write);
    std::cout << service.port() << " " << read.token << " " << write.token << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
    }
    service.stop();
    return 0;
}
