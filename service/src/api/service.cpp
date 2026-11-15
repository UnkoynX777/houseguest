#include "service.hpp"

#include <algorithm>
#include <fstream>

#include "houseguest/system/crypto.hpp"
#include "houseguest/system/https.hpp"

#include "../command_support.hpp"

namespace houseguest::service::api {
namespace {

using core::Json;
constexpr auto kSaveInterval = std::chrono::minutes(5);

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

}  // namespace

ApiService::ApiService(Host& host, const std::filesystem::path& directory)
    : host_(host),
      file_(directory / L"api-devices.json"),
      gate_(
          [](const std::string& url) -> std::optional<std::string> {
              system::HttpsRequest request;
              request.url = url;
              request.max_response = std::size_t{256} * 1024;
              const auto response = system::https_request(request);
              if (!response.ok()) return std::nullopt;
              return response.body;
          },
          [] { return now_seconds(); }) {
    load();
}

ApiService::~ApiService() {
    stop();
}

void ApiService::load() {
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    const Json wrapper = Json::parse(in, nullptr, false);
    if (!wrapper.is_object() || !wrapper.contains("data") || !wrapper.contains("mac") ||
        !wrapper["mac"].is_string())
        return;
    const auto data = wrapper["data"].dump();
    const auto key = host_.key();
    const auto expected =
        system::hmac_sha256(key, {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
    const auto given = system::from_hex(wrapper["mac"].get<std::string>());
    if (!given || !system::constant_time_equal(expected, *given)) return;  // tampered: start with no devices
    try {
        devices_.load(wrapper["data"]);
    } catch (const std::exception&) {
        devices_.load(Json::array());
    }
}

void ApiService::save() {
    std::scoped_lock lock(save_mutex_);
    const auto data = devices_.to_json();
    const auto text = data.dump();
    const auto mac =
        system::hmac_sha256(host_.key(), {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    const Json wrapper{{"data", data}, {"mac", system::to_hex(mac)}};
    auto temp = file_;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << wrapper.dump();
    }
    std::error_code ignored;
    std::filesystem::rename(temp, file_, ignored);
    last_saved_ = std::chrono::steady_clock::now();
}

void ApiService::save_if_due() {
    {
        std::scoped_lock lock(save_mutex_);
        if (std::chrono::steady_clock::now() - last_saved_ < kSaveInterval) return;
    }
    save();
}

bool ApiService::start(std::string& error) {
    if (running()) return true;
    listener_ = std::make_unique<Listener>([this](const Request& request) {
        const auto router = router_.load();
        if (!router) {
            Response unavailable;
            unavailable.status = 503;
            unavailable.body = R"({"ok":false,"error":{"code":"starting","message":"The API is starting."}})";
            return unavailable;
        }
        auto response = router->handle(request);
        save_if_due();
        return response;
    });
    if (!listener_->start(error)) {
        listener_.reset();
        return false;
    }
    allow_hostname({});  // builds the router for the loopback names
    return true;
}

void ApiService::allow_hostname(const std::string& hostname) {
    if (!listener_) return;
    {
        std::scoped_lock lock(hosts_mutex_);
        const auto name = lower(hostname);
        if (!name.empty() && std::ranges::find(extra_hosts_, name) == extra_hosts_.end())
            extra_hosts_.push_back(name);
    }
    RouterConfig config;
    const auto port = std::to_string(listener_->port());
    config.hosts = {"127.0.0.1:" + port, "localhost:" + port};
    {
        std::scoped_lock lock(hosts_mutex_);
        for (const auto& name : extra_hosts_) {
            config.hosts.insert(name);
            config.origins.insert("https://" + name);
        }
    }
    config.devices = &devices_;
    config.assets = panel_assets();
    config.precheck = [this](const Request& request) { return gate_.check(request); };
    config.gateway = [this](const std::string& command, const Json& args, const Device& device) {
        return host_.run(command, args, device.id);
    };
    config.pin_check = [this](const std::string& pin) { return host_.check_pin(pin); };
    config.audit = [this](const std::string& actor, const std::string& event, Json detail) {
        host_.audit(actor, event, std::move(detail));
    };
    config.health = [this] { return host_.health(); };
    router_.store(std::make_shared<Router>(std::move(config)));
}

void ApiService::stop() {
    if (!listener_) return;
    listener_->stop();
    router_.store(nullptr);
    if (devices_.revision() != 0) save();
    listener_.reset();
}

Issued ApiService::pair(const std::string& name, Scope scope) {
    auto issued = devices_.issue(name, scope, now_seconds());
    save();
    return issued;
}

bool ApiService::revoke(const std::string& id) {
    const bool changed = devices_.revoke(id);
    if (changed) save();
    return changed;
}

void ApiService::revoke_all() {
    devices_.revoke_all();
    save();
}

Json ApiService::devices_json() const {
    Json rows = Json::array();
    for (const auto& d : devices_.list())
        rows.push_back({{"id", d.id},
                        {"name", d.name},
                        {"scope", scope_name(d.scope)},
                        {"created", d.created},
                        {"expires", d.expires},
                        {"last_seen", d.last_seen},
                        {"revoked", d.revoked}});
    return rows;
}

Json ApiService::info() const {
    return Json{{"running", running()},
                {"port", port()},
                {"bind", "127.0.0.1"},
                {"served", listener_ ? listener_->served() : 0},
                {"refused", listener_ ? listener_->refused() : 0},
                {"devices", devices_json()}};
}

}  // namespace houseguest::service::api
