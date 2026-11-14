#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace houseguest::system {

enum class AppKind { Installed, Portable, Store, Manual };

enum class AppRole { Application, Tool, Helper, Installer };

struct InstalledApp {
    std::string name;
    std::string path;
    std::string company;
    std::string version;
    std::string description;
    std::string source;
    std::string package_family;
    std::string install_dir;
    std::vector<std::string> sources;
    AppKind kind = AppKind::Installed;
    AppRole role = AppRole::Application;
    std::string scope;
    std::int64_t modified = 0;
    std::int64_t size = 0;
};

struct SourceReport {
    std::string name;
    bool available = true;
    std::size_t found = 0;
    std::int64_t milliseconds = 0;
    std::string error;
};

struct CatalogRequest {
    std::vector<std::filesystem::path> portable_roots;
    std::vector<std::filesystem::path> manual;
    std::function<void(const std::vector<InstalledApp>&)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

struct Catalog {
    std::vector<InstalledApp> apps;
    std::vector<SourceReport> sources;
    bool cancelled = false;
};

const char* kind_name(AppKind kind);
const char* role_name(AppRole role);

Catalog discover_catalog(const CatalogRequest& request);

std::vector<SourceReport> probe_sources();

InstalledApp describe_executable(const std::filesystem::path& file);

AppRole classify_executable(const std::filesystem::path& file, const std::string& name);

std::vector<InstalledApp> scan_folder(const std::filesystem::path& folder, unsigned depth,
                                      const std::atomic<bool>* cancel = nullptr);

std::string normalize_app_key(const std::filesystem::path& file);

}  // namespace houseguest::system
