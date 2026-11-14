#pragma once

#include <set>
#include <string>
#include <vector>

#include "houseguest/system/app_catalog.hpp"

#include "catalog_support.hpp"

namespace houseguest::system::catalog {

struct Claim {
    std::string source;
    std::string platform;
    std::string product_id;
    std::string name;
    std::string publisher;
    std::string version;
    std::string scope;
    std::string package_family;
    AppKind kind = AppKind::Installed;
    fs::path root;
    std::vector<std::pair<fs::path, int>> executables;
    std::vector<FileEntry> known_executables;
    std::vector<Evidence> evidence;
    int name_rank = 1;
    bool user_facing = true;
};

class ProviderContext {
public:
    ProviderContext(const CatalogRequest& request, const std::vector<VolumeInfo>& volumes,
                    const std::vector<std::string>& running)
        : request_(request), volumes_(volumes), running_(running) {}

    const CatalogRequest& request() const { return request_; }
    const std::vector<VolumeInfo>& volumes() const { return volumes_; }
    const std::vector<std::string>& running() const { return running_; }
    bool cancelled() const {
        return request_.cancel != nullptr && request_.cancel->load(std::memory_order_relaxed);
    }

    void claim(Claim claim);
    void seed(const std::vector<Claim>& earlier);
    void discard(const fs::path& path, std::string reason);
    bool covered(const std::string& key) const;
    bool volume_ready(const fs::path& path) const;

    std::vector<Claim> claims;
    std::vector<Discarded> discarded;
    std::size_t discard_count = 0;
    std::string source;
    std::string error;
    bool available = true;
    std::string note;

private:
    const CatalogRequest& request_;
    const std::vector<VolumeInfo>& volumes_;
    const std::vector<std::string>& running_;
    std::set<std::string> covered_;
};

struct Provider {
    const char* name;
    const char* note;
    bool deep_only;
    void (*run)(ProviderContext&);
};

const std::vector<Provider>& providers();

std::vector<Claim> group_installations(const fs::path& folder, const char* source,
                                       const std::atomic<bool>* cancel);

}  // namespace houseguest::system::catalog
