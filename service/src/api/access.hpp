#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "houseguest/system/jwt.hpp"

#include "http.hpp"

namespace houseguest::service::api {

struct AccessConfig {
    bool enabled = false;
    std::string team_domain;          // team.cloudflareaccess.com
    std::string audience;             // Access application AUD tag
    std::string hostname;             // public hostname served by the tunnel
    std::vector<std::string> emails;  // optional extra allowlist on top of the Access policy
};

// Decides whether a request that arrived through the tunnel carries a valid Cloudflare Access identity.
// The headers added by Cloudflare are only believed after the signed token is verified against the team keys;
// a request that merely claims to be local but carries proxy headers is treated as remote.
class AccessGate {
public:
    using Fetch = std::function<std::optional<std::string>(const std::string& url)>;
    using Clock = std::function<std::int64_t()>;

    AccessGate(Fetch fetch, Clock clock);

    void configure(AccessConfig config);
    [[nodiscard]] AccessConfig config() const;

    [[nodiscard]] static bool is_remote(const Request& request);
    [[nodiscard]] std::optional<std::string> check(const Request& request);

    static constexpr std::int64_t kKeyLifetime = 3600;
    static constexpr std::int64_t kRefreshGap = 60;

private:
    bool refresh(std::int64_t now);

    Fetch fetch_;
    Clock clock_;
    mutable std::mutex mutex_;
    AccessConfig config_;
    std::vector<system::JwtKey> keys_;
    std::int64_t fetched_ = 0;
    std::int64_t attempted_ = 0;
};

}  // namespace houseguest::service::api
