#include "devices.hpp"

#include <algorithm>
#include <stdexcept>

#include "houseguest/system/crypto.hpp"

#include "../command_support.hpp"

namespace houseguest::service::api {
namespace {

using core::Json;

std::string hash_token(const std::string& token) {
    const auto bytes = reinterpret_cast<const std::uint8_t*>(token.data());
    return system::to_hex(system::sha256({bytes, token.size()}));
}

}  // namespace

const char* scope_name(Scope scope) {
    return scope == Scope::Write ? "write" : "read";
}

Issued DeviceRegistry::issue(const std::string& name, Scope scope, std::int64_t now) {
    if (name.empty() || name.size() > 64)
        throw std::invalid_argument("Device name must be 1 to 64 characters.");
    std::scoped_lock lock(mutex_);
    const auto active =
        std::ranges::count_if(devices_, [&](const Device& d) { return !d.revoked && d.expires > now; });
    if (static_cast<std::size_t>(active) >= kMaxDevices)
        throw Failure("limit", "Revoke a device before pairing another one.");
    Issued issued;
    issued.token = "hgd_" + system::to_hex(system::random_bytes(32));
    issued.device = {random_id("dev"),      name, scope, hash_token(issued.token), now,
                     now + kDeviceLifetime, now,  false};
    devices_.push_back(issued.device);
    ++revision_;
    return issued;
}

std::optional<Device> DeviceRegistry::authenticate(const std::string& token, std::int64_t now) {
    if (token.size() != 68 || !token.starts_with("hgd_")) return std::nullopt;
    const auto hash = hash_token(token);
    std::scoped_lock lock(mutex_);
    Device* match = nullptr;
    for (auto& device : devices_) {
        // Compare every entry in constant time so the position of a match is not observable.
        const auto a = reinterpret_cast<const std::uint8_t*>(device.token_hash.data());
        const auto b = reinterpret_cast<const std::uint8_t*>(hash.data());
        if (device.token_hash.size() == hash.size() &&
            system::constant_time_equal({a, device.token_hash.size()}, {b, hash.size()}))
            match = &device;
    }
    if (!match || match->revoked || match->expires <= now || now - match->last_seen > kDeviceIdle)
        return std::nullopt;
    match->last_seen = now;
    return *match;
}

bool DeviceRegistry::revoke(const std::string& id) {
    std::scoped_lock lock(mutex_);
    for (auto& device : devices_) {
        if (device.id == id && !device.revoked) {
            device.revoked = true;
            ++revision_;
            return true;
        }
    }
    return false;
}

void DeviceRegistry::revoke_all() {
    std::scoped_lock lock(mutex_);
    for (auto& device : devices_) device.revoked = true;
    ++revision_;
}

std::vector<Device> DeviceRegistry::list() const {
    std::scoped_lock lock(mutex_);
    return devices_;
}

std::uint64_t DeviceRegistry::revision() const {
    std::scoped_lock lock(mutex_);
    return revision_;
}

Json DeviceRegistry::to_json() const {
    std::scoped_lock lock(mutex_);
    Json rows = Json::array();
    for (const auto& d : devices_)
        rows.push_back({{"id", d.id},
                        {"name", d.name},
                        {"scope", scope_name(d.scope)},
                        {"token_hash", d.token_hash},
                        {"created", d.created},
                        {"expires", d.expires},
                        {"last_seen", d.last_seen},
                        {"revoked", d.revoked}});
    return rows;
}

void DeviceRegistry::load(const Json& json) {
    std::vector<Device> loaded;
    if (json.is_array()) {
        for (const auto& row : json) {
            Device d;
            d.id = row.at("id").get<std::string>();
            d.name = row.at("name").get<std::string>();
            d.scope = row.at("scope").get<std::string>() == "write" ? Scope::Write : Scope::Read;
            d.token_hash = row.at("token_hash").get<std::string>();
            d.created = row.at("created").get<std::int64_t>();
            d.expires = row.at("expires").get<std::int64_t>();
            d.last_seen = row.at("last_seen").get<std::int64_t>();
            d.revoked = row.at("revoked").get<bool>();
            loaded.push_back(std::move(d));
            if (loaded.size() >= 4 * kMaxDevices) break;
        }
    }
    std::scoped_lock lock(mutex_);
    devices_ = std::move(loaded);
    ++revision_;
}

}  // namespace houseguest::service::api
