#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include "api/service.hpp"
#include "cloudflare/api.hpp"
#include "cloudflare/tunnel.hpp"

namespace houseguest::service {

struct RemoteConfig {
    bool configured = false;
    bool enabled = false;
    std::string hostname;
    std::string account_id;
    std::string zone_id;
    std::string tunnel_id;
    std::string tunnel_name;
    std::string access_app_id;
    std::string audience;
    std::string team_domain;
    std::vector<std::string> emails;
    std::string secret_base64;  // kept only DPAPI-protected on disk
};

// Remote access: one Cloudflare tunnel in front of the local API, always behind a Cloudflare Access policy.
// Nothing here is needed for enforcement, and nothing starts while remote access is off.
class RemoteAccess {
public:
    RemoteAccess(api::ApiService& api, std::filesystem::path directory, cloudflare::Transport transport,
                 cloudflare::Launcher launcher, std::vector<std::filesystem::path> program_folders);
    ~RemoteAccess();

    struct SetupRequest {
        std::string api_token;
        std::string account_id;
        std::string zone_id;
        std::string hostname;
        std::vector<std::string> emails;
    };

    [[nodiscard]] core::Json discover(const std::string& api_token);
    core::Json setup(const SetupRequest& request);
    core::Json enable();
    core::Json disable();
    // Removes the Cloudflare resources when a token is given; always forgets the local configuration.
    core::Json reset(const std::string& api_token);
    [[nodiscard]] core::Json status();
    [[nodiscard]] core::Json logs();
    core::Json test();
    core::Json install_component();
    void resume();
    void shutdown();

private:
    void load();
    void save();
    void apply_access(bool enabled);

    api::ApiService& api_;
    std::filesystem::path file_;
    cloudflare::Transport transport_;
    std::vector<std::filesystem::path> program_folders_;
    cloudflare::TunnelManager tunnel_;
    std::mutex mutex_;
    RemoteConfig config_;
};

[[nodiscard]] bool valid_email(const std::string& email);

}  // namespace houseguest::service
