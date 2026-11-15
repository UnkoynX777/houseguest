#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "api/listener.hpp"

using namespace houseguest::service::api;

namespace {

struct Winsock {
    Winsock() {
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
    }
    ~Winsock() { WSACleanup(); }
};

SOCKET connect_to(std::uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return s;
}

std::string exchange(std::uint16_t port, const std::string& request) {
    SOCKET s = connect_to(port);
    if (s == INVALID_SOCKET) return {};
    send(s, request.data(), static_cast<int>(request.size()), 0);
    std::string reply;
    char buffer[2048];
    for (int n; (n = recv(s, buffer, sizeof(buffer), 0)) > 0;)
        reply.append(buffer, static_cast<std::size_t>(n));
    closesocket(s);
    return reply;
}

Response echo(const Request& request) {
    Response response;
    response.body = request.method + " " + request.target + " " + request.body;
    response.content_type = "text/plain";
    return response;
}

}  // namespace

TEST_CASE("the listener takes a free port from Windows and serves on it", "[listener]") {
    Winsock winsock;
    Listener listener(echo);
    std::string error;
    REQUIRE(listener.start(error));
    CHECK(listener.port() != 0);
    const auto reply =
        exchange(listener.port(), "POST /v1/x HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nhi");
    CHECK(reply.starts_with("HTTP/1.1 200 OK"));
    CHECK(reply.ends_with("POST /v1/x hi"));
    listener.stop();
    CHECK_FALSE(listener.running());
}

TEST_CASE("listeners never share a port and the port is exclusive", "[listener][security]") {
    Winsock winsock;
    std::vector<std::unique_ptr<Listener>> all;
    std::set<std::uint16_t> ports;
    for (int i = 0; i < 8; ++i) {
        all.push_back(std::make_unique<Listener>(echo));
        std::string error;
        REQUIRE(all.back()->start(error));
        ports.insert(all.back()->port());
    }
    CHECK(ports.size() == 8);

    SOCKET intruder = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(*ports.begin());
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const BOOL reuse = TRUE;
    setsockopt(intruder, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    CHECK(bind(intruder, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0);
    closesocket(intruder);
}

TEST_CASE("the listener accepts only loopback connections", "[listener][security]") {
    Winsock winsock;
    Listener listener(echo);
    std::string error;
    REQUIRE(listener.start(error));
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(listener.port());
    char host[256] = {};
    gethostname(host, sizeof(host));
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    bool reached = false;
    if (getaddrinfo(host, nullptr, &hints, &found) == 0) {
        for (auto* p = found; p; p = p->ai_next) {
            auto address = *reinterpret_cast<sockaddr_in*>(p->ai_addr);
            if (address.sin_addr.S_un.S_un_b.s_b1 == 127) continue;
            address.sin_port = htons(listener.port());
            if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) reached = true;
        }
        freeaddrinfo(found);
    }
    closesocket(s);
    CHECK_FALSE(reached);
}

TEST_CASE("a listener can be restarted and gets a fresh socket", "[listener]") {
    Winsock winsock;
    Listener listener(echo);
    std::string error;
    REQUIRE(listener.start(error));
    listener.stop();
    REQUIRE(listener.start(error));
    CHECK(exchange(listener.port(), "GET /a HTTP/1.1\r\nHost: a\r\n\r\n").find("GET /a") !=
          std::string::npos);
}

TEST_CASE("malformed and oversized requests get an error, not a handler call", "[listener][security]") {
    Winsock winsock;
    std::atomic<int> calls = 0;
    Listener listener([&](const Request&) {
        ++calls;
        return Response{};
    });
    std::string error;
    REQUIRE(listener.start(error));
    CHECK(exchange(listener.port(), "GARBAGE\r\n\r\n").starts_with("HTTP/1.1 400"));
    CHECK(exchange(listener.port(), "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 99999999\r\n\r\n")
              .starts_with("HTTP/1.1 413"));
    CHECK(calls == 0);
}

TEST_CASE("a client that stalls is dropped within the budget", "[listener][security]") {
    Winsock winsock;
    ListenerLimits limits;
    limits.read_timeout_ms = 300;
    limits.request_budget_ms = 600;
    Listener listener(echo, limits);
    std::string error;
    REQUIRE(listener.start(error));
    SOCKET s = connect_to(listener.port());
    REQUIRE(s != INVALID_SOCKET);
    const char* partial = "GET / HTTP/1.1\r\nHost: a\r\n";
    send(s, partial, static_cast<int>(strlen(partial)), 0);
    char buffer[512];
    const auto started = std::chrono::steady_clock::now();
    const int n = recv(s, buffer, sizeof(buffer), 0);
    CHECK(n > 0);
    CHECK(std::string(buffer, static_cast<std::size_t>(n)).starts_with("HTTP/1.1 408"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    closesocket(s);
}

TEST_CASE("concurrent clients are served and the connection cap refuses the excess", "[listener]") {
    Winsock winsock;
    Listener listener(echo);
    std::string error;
    REQUIRE(listener.start(error));
    std::atomic<int> ok = 0;
    std::vector<std::thread> clients;
    for (int i = 0; i < 12; ++i)
        clients.emplace_back([&] {
            if (exchange(listener.port(), "GET /c HTTP/1.1\r\nHost: a\r\n\r\n").starts_with("HTTP/1.1 200"))
                ++ok;
        });
    for (auto& t : clients) t.join();
    CHECK(ok == 12);

    ListenerLimits tight;
    tight.max_connections = 1;
    tight.read_timeout_ms = 1500;
    Listener small(echo, tight);
    REQUIRE(small.start(error));
    SOCKET hold = connect_to(small.port());
    REQUIRE(hold != INVALID_SOCKET);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(exchange(small.port(), "GET / HTTP/1.1\r\nHost: a\r\n\r\n").starts_with("HTTP/1.1 503"));
    closesocket(hold);
    small.stop();
}
