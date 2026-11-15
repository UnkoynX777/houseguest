#include "remote.hpp"

#include <algorithm>
#include <fstream>
#include <regex>

#include "houseguest/system/crypto.hpp"

#include "cloudflare/process.hpp"
#include "command_support.hpp"

namespace houseguest::service {
namespace {

using core::Json;

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool plausible_id(const std::string& id) {
    static const std::regex pattern(R"(^[0-9A-Za-z-]{8,64}$)");
    return std::regex_match(id, pattern);
}

}  // namespace

bool valid_email(const std::string& email) {
    static const std::regex pattern(R"(^[A-Za-z0-9._%+-]{1,64}@[A-Za-z0-9.-]{1,190}\.[A-Za-z]{2,}$)");
    return email.size() <= 254 && std::regex_match(email, pattern);
}

RemoteAccess::RemoteAccess(api::ApiService& api, const std::filesystem::path& directory,
                           cloudflare::Transport transport, cloudflare::Launcher launcher,
                           std::vector<std::filesystem::path> program_folders)
    : api_(api),
      file_(directory / L"remote.json"),
      transport_(std::move(transport)),
      program_folders_(std::move(program_folders)),
      tunnel_(std::move(launcher), directory / L"remote") {
    load();
}

RemoteAccess::~RemoteAccess() {
    shutdown();
}

void RemoteAccess::load() {
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    const Json doc = Json::parse(in, nullptr, false);
    if (!doc.is_object()) return;
    try {
        RemoteConfig c;
        c.configured = doc.value("configured", false);
        c.enabled = doc.value("enabled", false);
        c.hostname = doc.value("hostname", "");
        c.account_id = doc.value("account_id", "");
        c.zone_id = doc.value("zone_id", "");
        c.tunnel_id = doc.value("tunnel_id", "");
        c.tunnel_name = doc.value("tunnel_name", "");
        c.access_app_id = doc.value("access_app_id", "");
        c.audience = doc.value("audience", "");
        c.team_domain = doc.value("team_domain", "");
        if (doc.contains("emails") && doc["emails"].is_array())
            for (const auto& e : doc["emails"])
                if (e.is_string()) c.emails.push_back(e.get<std::string>());
        if (const auto protected_secret = system::from_hex(doc.value("secret", ""))) {
            if (const auto secret = system::dpapi_unprotect(*protected_secret))
                c.secret_base64.assign(secret->begin(), secret->end());
        }
        if (c.configured && (!cloudflare::valid_hostname(c.hostname) || !plausible_id(c.tunnel_id) ||
                             c.secret_base64.empty()))
            c = {};  // damaged or tampered: forget it rather than run it
        config_ = std::move(c);
    } catch (const std::exception&) {
        config_ = {};
    }
}

void RemoteAccess::save() {
    const auto& c = config_;
    const system::Bytes secret(c.secret_base64.begin(), c.secret_base64.end());
    const Json doc{{"configured", c.configured},
                   {"enabled", c.enabled},
                   {"hostname", c.hostname},
                   {"account_id", c.account_id},
                   {"zone_id", c.zone_id},
                   {"tunnel_id", c.tunnel_id},
                   {"tunnel_name", c.tunnel_name},
                   {"access_app_id", c.access_app_id},
                   {"audience", c.audience},
                   {"team_domain", c.team_domain},
                   {"emails", c.emails},
                   {"secret", secret.empty() ? "" : system::to_hex(system::dpapi_protect_machine(secret))}};
    auto temp = file_;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << doc.dump();
    }
    std::error_code ignored;
    std::filesystem::rename(temp, file_, ignored);
}

void RemoteAccess::apply_access(bool enabled) {
    api_.access().configure(
        {enabled, config_.team_domain, config_.audience, config_.hostname, config_.emails});
}

Json RemoteAccess::discover(const std::string& api_token) {
    cloudflare::Api api(transport_, api_token);
    try {
        (void)api.verify_token();
        Json accounts = Json::array();
        for (const auto& a : api.accounts())
            accounts.push_back({{"id", a.value("id", "")}, {"name", a.value("name", "")}});
        Json zones = Json::array();
        for (const auto& z : api.zones())
            zones.push_back({{"id", z.value("id", "")},
                             {"name", z.value("name", "")},
                             {"account", z.contains("account") ? z["account"].value("id", "") : ""}});
        return Json{{"accounts", accounts}, {"zones", zones}};
    } catch (const cloudflare::ApiError& e) {
        throw Failure("cloudflare", e.what());
    }
}

Json RemoteAccess::setup(const SetupRequest& request) {
    std::scoped_lock lock(mutex_);
    if (config_.configured) throw Failure("conflict", "Remote access is already set up. Reset it first.");
    const auto hostname = lower(request.hostname);
    if (!cloudflare::valid_hostname(hostname)) throw Failure("bad_request", "The hostname is not valid.");
    if (!plausible_id(request.account_id) || !plausible_id(request.zone_id))
        throw Failure("bad_request", "Choose an account and a domain.");
    if (request.emails.empty() || request.emails.size() > 8)
        throw Failure("bad_request", "Name 1 to 8 e-mail addresses that may sign in.");
    std::vector<std::string> emails;
    for (const auto& e : request.emails) {
        if (!valid_email(e)) throw Failure("bad_request", "An e-mail address is not valid.");
        emails.push_back(lower(e));
    }
    if (request.api_token.size() < 20 || request.api_token.size() > 200)
        throw Failure("bad_request", "Paste a Cloudflare API token.");

    cloudflare::Api api(transport_, request.api_token);
    std::string tunnel_id, app_id;
    bool dns = false;
    try {
        (void)api.verify_token();
        const auto team = api.access_domain(request.account_id);
        const auto name = "Houseguest " + system::to_hex(system::random_bytes(3));
        const auto tunnel = api.create_tunnel(request.account_id, name);
        tunnel_id = tunnel.tunnel_id;
        api.create_dns_record(request.zone_id, hostname, tunnel.tunnel_id);
        dns = true;
        const auto app = api.create_access_app(request.account_id, "Houseguest", hostname, emails);
        app_id = app.id;

        RemoteConfig c;
        c.configured = true;
        c.enabled = false;
        c.hostname = hostname;
        c.account_id = request.account_id;
        c.zone_id = request.zone_id;
        c.tunnel_id = tunnel.tunnel_id;
        c.tunnel_name = name;
        c.access_app_id = app.id;
        c.audience = app.audience;
        c.team_domain = team;
        c.emails = emails;
        c.secret_base64 = tunnel.secret_base64;
        config_ = std::move(c);
        save();
    } catch (const cloudflare::ApiError& e) {
        try {
            if (!app_id.empty()) api.delete_access_app(request.account_id, app_id);
            if (dns) api.delete_dns_record(request.zone_id, hostname);
            if (!tunnel_id.empty()) api.delete_tunnel(request.account_id, tunnel_id);
        } catch (const cloudflare::ApiError&) {
        }
        throw Failure("cloudflare", e.what());
    }
    return Json{{"hostname", hostname}, {"enabled", false}};
}

Json RemoteAccess::install_component() {
    const auto folder = program_folders_.empty() ? std::filesystem::path() : program_folders_.back();
    std::string error;
    if (!cloudflare::install_cloudflared(transport_, folder, error)) throw Failure("install_failed", error);
    return Json{{"installed", true}};
}

Json RemoteAccess::enable() {
    std::scoped_lock lock(mutex_);
    if (!config_.configured) throw Failure("not_configured", "Set up remote access first.");
    if (config_.audience.empty() || config_.team_domain.empty() || config_.emails.empty())
        throw Failure("no_policy",
                      "Remote access needs a Cloudflare Access policy before it can be turned on.");
    if (!api_.running()) throw Failure("unavailable", "The local API is not running.");
    const auto exe = cloudflare::locate_cloudflared(program_folders_);
    if (!exe) throw Failure("install_required", "The official cloudflared component is not installed.");

    apply_access(true);
    api_.allow_hostname(config_.hostname);
    std::string error;
    if (tunnel_.state() != cloudflare::TunnelState::Stopped &&
        tunnel_.state() != cloudflare::TunnelState::Failed) {
        tunnel_.stop();
    }
    if (!tunnel_.start(*exe, {config_.tunnel_id, config_.account_id, config_.secret_base64, config_.hostname},
                       api_.port(), error)) {
        apply_access(false);
        throw Failure("start_failed", error);
    }
    config_.enabled = true;
    save();
    return Json{{"enabled", true}};
}

Json RemoteAccess::disable() {
    std::scoped_lock lock(mutex_);
    tunnel_.stop();
    apply_access(false);
    config_.enabled = false;
    if (config_.configured) save();
    return Json{{"enabled", false}};
}

Json RemoteAccess::reset(const std::string& api_token) {
    std::scoped_lock lock(mutex_);
    tunnel_.stop();
    apply_access(false);
    Json removed = Json::array();
    if (config_.configured && !api_token.empty()) {
        cloudflare::Api api(transport_, api_token);
        try {
            api.delete_access_app(config_.account_id, config_.access_app_id);
            removed.push_back("access");
            api.delete_dns_record(config_.zone_id, config_.hostname);
            removed.push_back("dns");
            api.delete_tunnel(config_.account_id, config_.tunnel_id);
            removed.push_back("tunnel");
        } catch (const cloudflare::ApiError& e) {
            throw Failure("cloudflare", std::string(e.what()) + " Nothing local was changed.");
        }
    }
    config_ = {};
    save();
    return Json{{"removed", removed}};
}

void RemoteAccess::resume() {
    bool wanted = false;
    {
        std::scoped_lock lock(mutex_);
        wanted = config_.configured && config_.enabled;
        if (!wanted) apply_access(false);
    }
    if (wanted) {
        try {
            enable();
        } catch (const Failure&) {
            // The tunnel stays off; the status shows why.
        }
    }
}

void RemoteAccess::shutdown() {
    tunnel_.stop();
}

Json RemoteAccess::status() {
    std::scoped_lock lock(mutex_);
    const auto exe = cloudflare::locate_cloudflared(program_folders_);
    Json status = tunnel_.status();
    status["configured"] = config_.configured;
    status["enabled"] = config_.enabled;
    status["hostname"] = config_.hostname;
    status["tunnel_name"] = config_.tunnel_name;
    status["emails"] = config_.emails;
    status["access_policy"] = !config_.audience.empty() && !config_.team_domain.empty();
    status["component_installed"] = exe.has_value();
    status["url"] = config_.configured ? "https://" + config_.hostname : "";
    return status;
}

Json RemoteAccess::logs() {
    return Json(tunnel_.logs());
}

Json RemoteAccess::test() {
    std::string hostname;
    {
        std::scoped_lock lock(mutex_);
        if (!config_.configured) throw Failure("not_configured", "Set up remote access first.");
        hostname = config_.hostname;
    }
    system::HttpsRequest request;
    request.url = "https://" + hostname + "/v1/health";
    request.timeout_ms = 10000;
    request.max_response = std::size_t{64} * 1024;
    const auto response = transport_(request);
    if (response.status == 0)
        return Json{{"reachable", false}, {"protected", false}, {"message", response.error}};
    const Json body = Json::parse(response.body, nullptr, false);
    const bool exposed = body.is_object() && body.value("ok", false) == true;
    return Json{
        {"reachable", true},
        {"protected", !exposed},
        {"status", response.status},
        {"message", exposed ? "The API answered without a Cloudflare Access sign-in. Turn remote access off."
                            : "Anonymous visitors are stopped before they reach Houseguest."}};
}

}  // namespace houseguest::service
