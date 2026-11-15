#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"

namespace houseguest::service::api {

enum class Scope { Read, Write };

struct Device {
    std::string id;
    std::string name;
    Scope scope = Scope::Read;
    std::string token_hash;  // SHA-256 of the token, hex; the token itself is never stored
    std::int64_t created = 0;
    std::int64_t expires = 0;
    std::int64_t last_seen = 0;
    bool revoked = false;
};

inline constexpr std::int64_t kDeviceLifetime = 30LL * 24 * 3600;
inline constexpr std::int64_t kDeviceIdle = 12LL * 3600;
inline constexpr std::size_t kMaxDevices = 16;

struct Issued {
    Device device;
    std::string token;  // shown once
};

// Devices paired by the owner. A token is 256 random bits; only its hash is kept, so a copy of the registry
// file cannot be replayed. Tokens expire after a fixed lifetime and after a period without use.
class DeviceRegistry {
public:
    Issued issue(const std::string& name, Scope scope, std::int64_t now);
    [[nodiscard]] std::optional<Device> authenticate(const std::string& token, std::int64_t now);
    bool revoke(const std::string& id);
    void revoke_all();
    [[nodiscard]] std::vector<Device> list() const;

    [[nodiscard]] core::Json to_json() const;
    void load(const core::Json& json);
    [[nodiscard]] std::uint64_t revision() const;

private:
    mutable std::mutex mutex_;
    std::vector<Device> devices_;
    std::uint64_t revision_ = 0;
};

[[nodiscard]] const char* scope_name(Scope scope);

}  // namespace houseguest::service::api
