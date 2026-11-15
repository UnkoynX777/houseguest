#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "access.hpp"
#include "devices.hpp"
#include "listener.hpp"
#include "router.hpp"

namespace houseguest::service::api {

// What the API needs from the service. The service implements it; nothing here reaches the system directly.
class Host {
public:
    virtual ~Host() = default;
    virtual core::Json run(const std::string& command, const core::Json& args, const std::string& device_id) = 0;
    virtual bool check_pin(const std::string& pin) = 0;
    virtual void audit(const std::string& actor, const std::string& event, core::Json detail) = 0;
    virtual core::Json health() = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> key() const = 0;
};

// Owns the loopback listener, the router and the paired devices. Devices are kept in a file protected by an HMAC
// under a key derived from the service state key; a file that fails the check is discarded, never trusted.
class ApiService {
public:
    ApiService(Host& host, std::filesystem::path directory);
    ~ApiService();

    bool start(std::string& error);
    void stop();

    [[nodiscard]] bool running() const { return listener_ && listener_->running(); }
    [[nodiscard]] std::uint16_t port() const { return listener_ ? listener_->port() : 0; }
    [[nodiscard]] core::Json info() const;

    Issued pair(const std::string& name, Scope scope);
    bool revoke(const std::string& id);
    void revoke_all();
    [[nodiscard]] core::Json devices_json() const;
    // Lets the Cloudflare layer register the public hostname and its allowed origin once a tunnel is configured.
    void allow_hostname(const std::string& hostname);
    [[nodiscard]] AccessGate& access() { return gate_; }

private:
    void load();
    void save();
    void save_if_due();

    Host& host_;
    std::filesystem::path file_;
    DeviceRegistry devices_;
    AccessGate gate_;
    std::unique_ptr<Listener> listener_;
    std::atomic<std::shared_ptr<Router>> router_;
    std::mutex save_mutex_;
    std::chrono::steady_clock::time_point last_saved_ = std::chrono::steady_clock::now();
    std::mutex hosts_mutex_;
    std::vector<std::string> extra_hosts_;
};

}  // namespace houseguest::service::api
