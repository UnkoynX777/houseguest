#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include "../ipc_limits.hpp"
#include "devices.hpp"
#include "assets.hpp"
#include "http.hpp"

namespace houseguest::service::api {

// Runs one named service command on behalf of an authenticated device. It throws service Failure for refusals.
using Gateway = std::function<core::Json(const std::string& command, const core::Json& args, const Device& device)>;
// Checks the PIN for a step-up confirmation. Returns false when it does not match.
using PinCheck = std::function<bool(const std::string& pin)>;
// Extra gate run before authentication, for example the Cloudflare Access identity check. Returns a refusal reason.
using Precheck = std::function<std::optional<std::string>(const Request&)>;
using Clock = std::function<std::int64_t()>;
using Audit = std::function<void(const std::string& actor, const std::string& event, core::Json detail)>;

struct RouterConfig {
    std::set<std::string> hosts;    // accepted Host header values (lower case)
    std::set<std::string> origins;  // accepted Origin values; a request without Origin is allowed
    Gateway gateway;
    PinCheck pin_check;
    Precheck precheck;
    Audit audit;
    Clock clock;
    DeviceRegistry* devices = nullptr;
    std::function<core::Json()> health;
    std::span<const Asset> assets;
};

enum class Risk { Read, Change, Sensitive };

struct Route {
    const char* method;
    const char* path;
    const char* command;
    Risk risk;
};

class Router {
public:
    explicit Router(RouterConfig config);
    [[nodiscard]] Response handle(const Request& request);

    static constexpr std::int64_t kSkewSeconds = 60;
    static constexpr std::int64_t kStepupSeconds = 300;

private:
    [[nodiscard]] Response reply(int status, const core::Json& body, const Request& request) const;
    [[nodiscard]] Response failure(int status, const std::string& code, const std::string& message,
                                   const Request& request) const;
    [[nodiscard]] bool fresh_nonce(const std::string& nonce, std::int64_t now);
    [[nodiscard]] bool take_device_slot(const std::string& id);
    [[nodiscard]] bool take_failure_slot();
    void cors(Response& response, const Request& request) const;

    RouterConfig config_;
    std::mutex mutex_;
    std::deque<std::pair<std::int64_t, std::string>> nonce_order_;
    std::set<std::string> nonces_;
    std::unordered_map<std::string, TokenBucket> buckets_;
    TokenBucket failures_{3.0, 10.0};
    std::map<std::string, std::int64_t> stepup_;
    std::map<std::string, std::pair<std::int64_t, int>> pin_failures_;
};

}  // namespace houseguest::service::api
