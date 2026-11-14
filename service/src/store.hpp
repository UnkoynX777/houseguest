#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/crypto.hpp"

namespace houseguest::service {

struct PinRecord {
    std::string salt_hex;
    std::string hash_hex;
    std::uint32_t iterations = 0;
};

struct Session {
    std::string profile_id;
    std::vector<std::string> guest_sids;
    std::int64_t started_at = 0;
    std::int64_t ends_at = 0;
    std::string on_expiry = "disconnect";
    bool audit_only = false;
    std::vector<core::Rule> exceptions;
};

struct State {
    std::string owner_sid;
    PinRecord pin;
    int failed_attempts = 0;
    std::int64_t locked_until = 0;
    std::string language = "en";
    std::string default_on_expiry = "disconnect";
    std::vector<core::Profile> profiles;
    std::optional<Session> session;

    [[nodiscard]] bool configured() const { return !owner_sid.empty(); }
};

enum class LoadResult { Loaded, Fresh, Recovered, Tampered };

class Store {
public:
    explicit Store(std::filesystem::path directory);

    LoadResult load();
    void save();
    std::filesystem::path create_backup();
    bool restore_backup(const std::string& name);
    std::vector<std::string> list_backups() const;

    State state;

    const std::filesystem::path& directory() const { return directory_; }

private:
    std::optional<State> read_verified(const std::filesystem::path& file) const;

    std::filesystem::path directory_;
    system::Bytes mac_key_;
};

State state_from_json(const core::Json& json);
core::Json state_to_json(const State& state);

void restrict_to_administrators(const std::filesystem::path& directory);

}  // namespace houseguest::service
