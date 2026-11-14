#pragma once

#include <atomic>
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
    std::string phase = "active";
    std::string end_reason;
    bool end_apply_action = false;

    [[nodiscard]] bool ending() const { return phase == "ending"; }
};

struct ManagedAccount {
    std::string name;
    std::string sid;
};

struct ManualApp {
    std::string path;
    std::string name;
    std::int64_t added_at = 0;
};

struct State {
    std::string owner_sid;
    PinRecord pin;
    int failed_attempts = 0;
    std::int64_t locked_until = 0;
    std::string language = "en";
    std::string default_on_expiry = "disconnect";
    std::vector<core::Profile> profiles;
    std::vector<core::Application> library;
    std::vector<ManagedAccount> managed;
    std::vector<ManualApp> manual_apps;
    std::vector<std::string> portable_roots;
    std::map<std::string, std::string> resolutions;
    bool include_removable = false;
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

    // Increases whenever the persisted state is written or replaced, so anything derived from it can tell
    // whether it was computed from the current policy.
    [[nodiscard]] std::uint64_t revision() const { return revision_.load(); }

    const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] system::Bytes derive_key(const std::string& label) const;

private:
    std::optional<State> read_verified(const std::filesystem::path& file) const;

    std::filesystem::path directory_;
    system::Bytes mac_key_;
    std::atomic<std::uint64_t> revision_{1};
};

State state_from_json(const core::Json& json);
core::Json state_to_json(const State& state);

void restrict_to_administrators(const std::filesystem::path& directory);

}  // namespace houseguest::service
