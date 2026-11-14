#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace houseguest::system {

enum class AppKind { Installed, Portable, Store, Manual };

enum class AppRole { Application, Tool, Helper, Installer };

enum class Resolution { Resolved, Candidates, Unresolved };

struct Evidence {
    std::string kind;
    std::string detail;
};

struct ExecutableCandidate {
    std::string path;
    int score = 0;
    std::vector<std::string> reasons;
};

struct InstalledApp {
    std::string id;
    std::string name;
    std::string path;
    std::string company;
    std::string version;
    std::string description;
    std::string source;
    std::vector<std::string> sources;
    std::string package_family;
    std::string install_dir;
    std::string platform;
    std::string product_id;
    std::string volume;
    std::string scope;
    AppKind kind = AppKind::Installed;
    AppRole role = AppRole::Application;
    Resolution resolution = Resolution::Resolved;
    std::vector<ExecutableCandidate> candidates;
    std::vector<Evidence> evidence;
    std::int64_t modified = 0;
    std::int64_t size = 0;
    std::int64_t scanned_at = 0;
    bool offline = false;
};

struct Discarded {
    std::string path;
    std::string reason;
    std::string source;
};

struct SourceReport {
    std::string name;
    std::string note;
    bool available = true;
    std::size_t found = 0;
    std::size_t unique = 0;
    std::size_t merged = 0;
    std::size_t discarded = 0;
    std::int64_t milliseconds = 0;
    std::string error;
};

struct VolumeInfo {
    std::string root;
    std::string type;
    std::string label;
    std::string filesystem;
    std::uint32_t serial = 0;
    bool ready = false;
    bool included = false;
    std::string reason;
};

struct CatalogRequest {
    std::vector<std::filesystem::path> portable_roots;
    std::vector<std::filesystem::path> manual;
    std::map<std::string, std::string> resolutions;
    std::vector<InstalledApp> previous;
    bool include_removable = false;
    bool deep_scan = false;
    std::function<void(const std::vector<InstalledApp>&)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct Catalog {
    std::vector<InstalledApp> apps;
    std::vector<SourceReport> sources;
    std::vector<VolumeInfo> volumes;
    std::vector<Discarded> discarded;
    bool cancelled = false;
};

const char* kind_name(AppKind kind);
const char* role_name(AppRole role);
const char* resolution_name(Resolution resolution);

Catalog discover_catalog(const CatalogRequest& request);

std::vector<VolumeInfo> enumerate_volumes(bool include_removable);

std::vector<SourceReport> describe_sources();

InstalledApp describe_executable(const std::filesystem::path& file);

AppRole classify_executable(const std::filesystem::path& file, const std::string& name);

std::vector<InstalledApp> scan_folder(const std::filesystem::path& folder,
                                      const std::atomic<bool>* cancel = nullptr);

std::string normalize_app_key(const std::filesystem::path& file);

}  // namespace houseguest::system
