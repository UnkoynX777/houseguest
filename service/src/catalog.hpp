#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/app_catalog.hpp"

namespace houseguest::service {

struct CatalogView {
    std::vector<system::InstalledApp> apps;
    std::vector<system::SourceReport> sources;
    std::vector<system::VolumeInfo> volumes;
    std::vector<system::Discarded> discarded;
    std::map<std::string, std::int64_t> first_seen;
    bool scanning = false;
    bool deep = false;
    std::int64_t updated_at = 0;
    std::int64_t scan_ms = 0;
    std::uint64_t generation = 0;
};

class AppCatalog {
public:
    explicit AppCatalog(std::filesystem::path cache_file);
    ~AppCatalog();

    AppCatalog(const AppCatalog&) = delete;
    AppCatalog& operator=(const AppCatalog&) = delete;

    bool refresh(system::CatalogRequest request);
    bool refresh_if_stale(std::chrono::seconds age, const system::CatalogRequest& request);
    void cancel();
    void request_stop() { cancel_ = true; }
    [[nodiscard]] CatalogView view() const;
    [[nodiscard]] bool scanning() const { return scanning_.load(); }
    [[nodiscard]] bool volumes_changed(bool include_removable) const;

private:
    void load();
    void persist(const CatalogView& view) const;
    void run(system::CatalogRequest request);
    void scan(system::CatalogRequest request);

    std::filesystem::path cache_file_;
    mutable std::mutex mutex_;
    CatalogView view_;
    std::vector<system::InstalledApp> committed_;
    std::string volume_signature_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::optional<system::CatalogRequest> pending_;
    bool baseline_ = true;
    std::jthread worker_;
};

core::Json to_json(const system::InstalledApp& app);
system::InstalledApp app_from_json(const core::Json& json);
core::Json to_json(const system::VolumeInfo& volume);
core::Json to_json(const system::SourceReport& report);

}  // namespace houseguest::service
