#include <winsock2.h>
#include <ws2tcpip.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "houseguest/system/crypto.hpp"

#include "api/service.hpp"

using namespace houseguest::service;
using namespace houseguest::service::api;
using houseguest::core::Json;

namespace {

struct FakeHost final : Host {
    std::vector<std::string> ran;
    std::vector<std::string> audited;
    Json run(const std::string& command, const Json&, const std::string&) override {
        ran.push_back(command);
        return Json{{"command", command}};
    }
    bool check_pin(const std::string& pin) override { return pin == "1357"; }
    void audit(const std::string&, const std::string& event, Json) override { audited.push_back(event); }
    Json health() override { return Json{{"version", "test"}}; }
    [[nodiscard]] std::vector<std::uint8_t> key() const override { return std::vector<std::uint8_t>(32, 7); }
};

std::string http(std::uint16_t port, const std::string& method, const std::string& path,
                 const std::string& token, const std::string& host_header = {}) {
    WSADATA d{};
    WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::string reply;
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        DWORD timeout = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const auto host = host_header.empty() ? "127.0.0.1:" + std::to_string(port) : host_header;
        std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + host + "\r\n";
        if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
        request += "\r\n";
        send(s, request.data(), static_cast<int>(request.size()), 0);
        char buffer[4096];
        for (int n; (n = recv(s, buffer, sizeof(buffer), 0)) > 0;)
            reply.append(buffer, static_cast<std::size_t>(n));
    }
    closesocket(s);
    WSACleanup();
    return reply;
}

std::filesystem::path scratch(const char* name) {
    auto dir = std::filesystem::temp_directory_path() / (std::string("hg-api-") + name);
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("the API answers over loopback with a paired device only", "[api][integration]") {
    FakeHost host;
    const auto dir = scratch("e2e");
    ApiService service(host, dir);
    std::string error;
    REQUIRE(service.start(error));
    REQUIRE(service.port() != 0);

    CHECK(http(service.port(), "GET", "/v1/health", "").find("200 OK") != std::string::npos);
    CHECK(http(service.port(), "GET", "/v1/status", "").find("401") != std::string::npos);

    const auto issued = service.pair("phone", Scope::Read);
    const auto ok = http(service.port(), "GET", "/v1/status", issued.token);
    CHECK(ok.find("200 OK") != std::string::npos);
    CHECK(ok.find("\"command\":\"status\"") != std::string::npos);
    CHECK(http(service.port(), "GET", "/v1/status", issued.token, "attacker.example").find("403") !=
          std::string::npos);

    service.revoke(issued.device.id);
    CHECK(http(service.port(), "GET", "/v1/status", issued.token).find("401") != std::string::npos);
    service.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("paired devices survive a restart and a tampered file is discarded",
          "[api][integration][security]") {
    FakeHost host;
    const auto dir = scratch("persist");
    std::string token;
    {
        ApiService service(host, dir);
        token = service.pair("tablet", Scope::Write).token;
        std::string error;
        REQUIRE(service.start(error));
        service.stop();
    }
    {
        ApiService service(host, dir);
        std::string error;
        REQUIRE(service.start(error));
        CHECK(http(service.port(), "GET", "/v1/status", token).find("200 OK") != std::string::npos);
        service.stop();
    }
    const auto file = dir / L"api-devices.json";
    std::string text;
    {
        std::ifstream in(file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    const auto at = text.find("\"read\"");
    const auto flip = text.find("\"write\"");
    REQUIRE(flip != std::string::npos);
    (void)at;
    text.replace(flip, 7, "\"read\"\x20");  // a different scope without a valid MAC
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }
    ApiService service(host, dir);
    std::string error;
    REQUIRE(service.start(error));
    CHECK(http(service.port(), "GET", "/v1/status", token).find("401") != std::string::npos);
    service.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("the port changes between starts and nothing listens after stop", "[api][integration]") {
    FakeHost host;
    const auto dir = scratch("ports");
    ApiService service(host, dir);
    std::string error;
    REQUIRE(service.start(error));
    const auto first = service.port();
    service.stop();
    CHECK(http(first, "GET", "/v1/health", "").empty());
    REQUIRE(service.start(error));
    CHECK(service.port() != 0);
    CHECK_FALSE(service.info()["devices"].is_null());
    service.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("the remote panel is served with a strict content security policy",
          "[api][integration][security]") {
    FakeHost host;
    const auto dir = scratch("panel");
    ApiService service(host, dir);
    std::string error;
    REQUIRE(service.start(error));
    const auto page = http(service.port(), "GET", "/", "");
    CHECK(page.find("200 OK") != std::string::npos);
    CHECK(page.find("Houseguest Remote") != std::string::npos);
    CHECK(page.find("Content-Security-Policy: default-src 'none'") != std::string::npos);
    CHECK(page.find("X-Frame-Options: DENY") != std::string::npos);
    const auto script = http(service.port(), "GET", "/panel.js", "");
    CHECK(script.find("text/javascript") != std::string::npos);
    CHECK(script.find("textContent") != std::string::npos);
    CHECK(script.find("innerHTML") == std::string::npos);
    CHECK(http(service.port(), "GET", "/panel.js", "", "attacker.example").find("403") != std::string::npos);
    service.stop();
    std::filesystem::remove_all(dir);
}

TEST_CASE("requests that arrive through the tunnel are refused while remote access is off",
          "[api][integration][security]") {
    FakeHost host;
    const auto dir = scratch("gate");
    ApiService service(host, dir);
    std::string error;
    REQUIRE(service.start(error));
    const auto issued = service.pair("phone", Scope::Read);
    WSADATA d{};
    WSAStartup(MAKEWORD(2, 2), &d);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(service.port());
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
    const std::string request =
        "GET /v1/health HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(service.port()) +
        "\r\nCf-Ray: abc\r\nAuthorization: Bearer " + issued.token + "\r\n\r\n";
    send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string reply;
    char buffer[2048];
    for (int n; (n = recv(s, buffer, sizeof(buffer), 0)) > 0;)
        reply.append(buffer, static_cast<std::size_t>(n));
    closesocket(s);
    WSACleanup();
    CHECK(reply.find("403") != std::string::npos);
    CHECK(reply.find("identity_required") != std::string::npos);
    service.stop();
    std::filesystem::remove_all(dir);
}
