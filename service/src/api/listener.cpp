#include "listener.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>

namespace houseguest::service::api {
namespace {

bool send_all(SOCKET s, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

Response error_response(int status, const std::string& message) {
    Response response;
    response.status = status;
    response.body = std::string(R"({"ok":false,"error":{"code":"http_)") + std::to_string(status) +
                    R"(","message":")" + message + R"("}})";
    return response;
}

}  // namespace

Listener::Listener(Handler handler, ListenerLimits limits) : handler_(std::move(handler)), limits_(limits) {}

Listener::~Listener() { stop(); }

bool Listener::start(std::string& error) {
    if (running()) return true;
    WSADATA data{};
    if (const int rc = WSAStartup(MAKEWORD(2, 2), &data); rc != 0) {
        error = "WSAStartup failed: " + std::to_string(rc);
        return false;
    }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = "socket failed: " + std::to_string(WSAGetLastError());
        WSACleanup();
        return false;
    }
    auto abandon = [&](const char* what) {
        error = std::string(what) + " failed: " + std::to_string(WSAGetLastError());
        closesocket(s);
        WSACleanup();
        return false;
    };
    const BOOL exclusive = TRUE;
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0)
        return abandon("SO_EXCLUSIVEADDRUSE");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return abandon("bind");
    int length = sizeof(address);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) != 0) return abandon("getsockname");
    if (listen(s, 16) != 0) return abandon("listen");

    port_ = ntohs(address.sin_port);
    socket_ = static_cast<std::uintptr_t>(s);
    stopping_ = false;
    acceptor_ = std::thread([this] { accept_loop(); });
    return true;
}

void Listener::stop() {
    if (!running()) return;
    stopping_ = true;
    const auto s = static_cast<SOCKET>(socket_.exchange(invalid_));
    closesocket(s);  // wakes accept()
    if (acceptor_.joinable()) acceptor_.join();
    {
        std::unique_lock lock(mutex_);
        idle_.wait(lock, [this] { return active_ == 0; });
    }
    port_ = 0;
    WSACleanup();
}

void Listener::accept_loop() {
    for (;;) {
        const auto listening = static_cast<SOCKET>(socket_.load());
        if (stopping_ || listening == INVALID_SOCKET) return;
        const SOCKET client = accept(listening, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (stopping_) return;
            Sleep(10);
            continue;
        }
        bool admitted = false;
        {
            std::scoped_lock lock(mutex_);
            if (active_ < limits_.max_connections) {
                ++active_;
                admitted = true;
            }
        }
        if (!admitted) {
            ++refused_;
            send_all(client, serialize(error_response(503, "Too many connections.")));
            closesocket(client);
            continue;
        }
        try {
            std::thread([this, client] { serve(static_cast<std::uintptr_t>(client)); }).detach();
        } catch (...) {
            closesocket(client);
            std::scoped_lock lock(mutex_);
            --active_;
            idle_.notify_all();
        }
    }
}

void Listener::serve(std::uintptr_t handle) {
    const auto client = static_cast<SOCKET>(handle);
    const DWORD timeout = static_cast<DWORD>(limits_.read_timeout_ms);
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    Response response;
    try {
        std::string buffer;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits_.request_budget_ms);
        ParseResult parsed;
        for (;;) {
            parsed = parse_request(buffer);
            if (parsed.status != ParseStatus::NeedMore) break;
            if (std::chrono::steady_clock::now() > deadline) {
                parsed.status = ParseStatus::Invalid;
                parsed.http_status = 408;
                parsed.reason = "Request took too long.";
                break;
            }
            char chunk[4096];
            const int n = recv(client, chunk, sizeof(chunk), 0);
            if (n == 0) {
                parsed.status = ParseStatus::Invalid;
                parsed.http_status = 400;
                parsed.reason = "Connection closed early.";
                break;
            }
            if (n < 0) {
                parsed.status = ParseStatus::Invalid;
                parsed.http_status = 408;
                parsed.reason = "Request timed out.";
                break;
            }
            buffer.append(chunk, static_cast<std::size_t>(n));
        }
        if (parsed.status == ParseStatus::Complete) {
            response = handler_(parsed.request);
            ++served_;
        } else {
            response = error_response(parsed.http_status, parsed.reason);
        }
    } catch (...) {
        response = error_response(500, "Internal error.");
    }
    send_all(client, serialize(response));
    shutdown(client, SD_SEND);
    closesocket(client);
    std::scoped_lock lock(mutex_);
    --active_;
    idle_.notify_all();
}

}  // namespace houseguest::service::api
