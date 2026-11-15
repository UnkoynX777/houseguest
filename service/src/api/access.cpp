#include "access.hpp"

#include <algorithm>
#include <cctype>

namespace houseguest::service::api {
namespace {

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

AccessGate::AccessGate(Fetch fetch, Clock clock) : fetch_(std::move(fetch)), clock_(std::move(clock)) {}

void AccessGate::configure(AccessConfig config) {
    std::scoped_lock lock(mutex_);
    config.team_domain = lower(config.team_domain);
    config.hostname = lower(config.hostname);
    for (auto& email : config.emails) email = lower(email);
    const bool same_team = config.team_domain == config_.team_domain;
    config_ = std::move(config);
    if (!same_team) {
        keys_.clear();
        fetched_ = 0;
        attempted_ = 0;
    }
}

AccessConfig AccessGate::config() const {
    std::scoped_lock lock(mutex_);
    return config_;
}

bool AccessGate::is_remote(const Request& request) {
    for (const auto& [name, value] : request.headers) {
        if (name.starts_with("cf-") || name.starts_with("x-forwarded-") || name == "forwarded" || name == "via" ||
            name == "x-real-ip" || name == "true-client-ip")
            return true;
    }
    return false;
}

bool AccessGate::refresh(std::int64_t now) {
    if (now - attempted_ < kRefreshGap) return false;
    attempted_ = now;
    if (!fetch_ || config_.team_domain.empty()) return false;
    const auto body = fetch_("https://" + config_.team_domain + "/cdn-cgi/access/certs");
    if (!body) return false;
    auto keys = system::parse_jwks(*body);
    if (keys.empty()) return false;
    keys_ = std::move(keys);
    fetched_ = now;
    return true;
}

std::optional<std::string> AccessGate::check(const Request& request) {
    if (!is_remote(request)) return std::nullopt;
    const auto now = clock_();
    std::scoped_lock lock(mutex_);
    if (!config_.enabled) return "Remote access is turned off.";
    if (config_.team_domain.empty() || config_.audience.empty())
        return "Remote access has no Cloudflare Access policy configured.";

    const auto token = std::string(request.header("cf-access-jwt-assertion"));
    if (token.empty()) return "A Cloudflare Access identity is required.";

    if (keys_.empty() || now - fetched_ > kKeyLifetime) refresh(now);
    system::JwtPolicy policy{"https://" + config_.team_domain, config_.audience, now, 60};
    auto result = system::verify_jwt(token, keys_, policy);
    if (!result.valid && result.reason == "unknown_key" && refresh(now)) result = system::verify_jwt(token, keys_, policy);
    if (!result.valid) return result.reason == "unknown_key" ? "The identity key is not recognised." : result.reason;
    if (!config_.emails.empty() && std::ranges::find(config_.emails, lower(result.email)) == config_.emails.end())
        return "This identity is not allowed.";
    return std::nullopt;
}

}  // namespace houseguest::service::api
