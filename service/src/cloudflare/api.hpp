#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/https.hpp"

namespace houseguest::service::cloudflare {

struct ApiError : std::runtime_error {
    explicit ApiError(std::string text, int http = 0) : std::runtime_error(std::move(text)), status(http) {}
    int status;
};

using Transport = std::function<system::HttpsResponse(const system::HttpsRequest&)>;

[[nodiscard]] bool valid_hostname(const std::string& hostname);

struct TunnelCredentials {
    std::string account_id;
    std::string tunnel_id;
    std::string secret_base64;
};

struct AccessApp {
    std::string id;
    std::string audience;
};

// Cloudflare REST client for the one-time setup of a tunnel, its DNS record and its Access policy. The API
// token is held only for the duration of the setup and never appears in an error message.
class Api {
public:
    Api(Transport transport, std::string token);

    [[nodiscard]] core::Json verify_token();
    [[nodiscard]] core::Json accounts();
    [[nodiscard]] core::Json zones();
    [[nodiscard]] std::string access_domain(const std::string& account_id);
    [[nodiscard]] TunnelCredentials create_tunnel(const std::string& account_id, const std::string& name);
    void delete_tunnel(const std::string& account_id, const std::string& tunnel_id);
    void create_dns_record(const std::string& zone_id, const std::string& hostname,
                           const std::string& tunnel_id);
    void delete_dns_record(const std::string& zone_id, const std::string& hostname);
    [[nodiscard]] AccessApp create_access_app(const std::string& account_id, const std::string& name,
                                              const std::string& hostname,
                                              const std::vector<std::string>& emails);
    void delete_access_app(const std::string& account_id, const std::string& app_id);
    [[nodiscard]] core::Json tunnel_status(const std::string& account_id, const std::string& tunnel_id);

private:
    core::Json call(const std::string& method, const std::string& path, const core::Json& body = nullptr);

    Transport transport_;
    std::string token_;
};

}  // namespace houseguest::service::cloudflare
