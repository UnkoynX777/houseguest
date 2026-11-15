#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "http.hpp"

namespace houseguest::service::api {

struct ListenerLimits {
    int max_connections = 16;
    int read_timeout_ms = 5000;     // between reads
    int request_budget_ms = 10000;  // whole request
};

// Loopback HTTP listener. The socket is bound once, to port 0, so Windows picks a free port and no other
// process can take it afterwards (SO_EXCLUSIVEADDRUSE); the port is read back with getsockname and the socket
// stays open for the lifetime of the listener, so there is no window between choosing and using the port.
class Listener {
public:
    using Handler = std::function<Response(const Request&)>;

    Listener(Handler handler, ListenerLimits limits = {});
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    bool start(std::string& error);
    void stop();

    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] bool running() const { return socket_.load() != invalid_; }
    [[nodiscard]] std::uint64_t served() const { return served_; }
    [[nodiscard]] std::uint64_t refused() const { return refused_; }

private:
    static constexpr std::uintptr_t invalid_ = ~std::uintptr_t{0};
    void accept_loop();
    void serve(std::uintptr_t client);

    Handler handler_;
    ListenerLimits limits_;
    std::atomic<std::uintptr_t> socket_ = invalid_;
    std::uint16_t port_ = 0;
    std::thread acceptor_;
    std::atomic<bool> stopping_ = false;
    std::mutex mutex_;
    std::condition_variable idle_;
    int active_ = 0;
    std::atomic<std::uint64_t> served_ = 0;
    std::atomic<std::uint64_t> refused_ = 0;
};

}  // namespace houseguest::service::api
