#include "catalog.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <unordered_set>

#include "houseguest/system/text.hpp"

namespace houseguest::service {
namespace {

constexpr int kCacheVersion = 1;

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

system::AppKind kind_from(const std::string& name) {
    if (name == "portable") return system::AppKind::Portable;
    if (name == "store") return system::AppKind::Store;
    if (name == "manual") return system::AppKind::Manual;
    return system::AppKind::Installed;
}

system::AppRole role_from(const std::string& name) {
    if (name == "tool") return system::AppRole::Tool;
    if (name == "helper") return system::AppRole::Helper;
    if (name == "installer") return system::AppRole::Installer;
    return system::AppRole::Application;
}

}  // namespace

core::Json to_json(const system::InstalledApp& app) {
    return {{"name", app.name},
            {"path", app.path},
            {"company", app.company},
            {"version", app.version},
            {"description", app.description},
            {"source", app.source},
            {"sources", app.sources},
            {"package_family", app.package_family},
            {"install_dir", app.install_dir},
            {"kind", system::kind_name(app.kind)},
            {"role", system::role_name(app.role)},
            {"scope", app.scope},
            {"modified", app.modified},
            {"size", app.size}};
}

system::InstalledApp app_from_json(const core::Json& json) {
    system::InstalledApp app;
    app.name = json.value("name", "");
    app.path = json.value("path", "");
    app.company = json.value("company", "");
    app.version = json.value("version", "");
    app.description = json.value("description", "");
    app.source = json.value("source", "");
    if (const auto it = json.find("sources"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) app.sources.push_back(item.get<std::string>());
        }
    }
    app.package_family = json.value("package_family", "");
    app.install_dir = json.value("install_dir", "");
    app.kind = kind_from(json.value("kind", ""));
    app.role = role_from(json.value("role", ""));
    app.scope = json.value("scope", "");
    app.modified = json.value("modified", std::int64_t{0});
    app.size = json.value("size", std::int64_t{0});
    return app;
}

AppCatalog::AppCatalog(std::filesystem::path cache_file) : cache_file_(std::move(cache_file)) {
    load();
}

AppCatalog::~AppCatalog() {
    cancel();
}

void AppCatalog::load() {
    std::ifstream in(cache_file_, std::ios::binary);
    if (!in) return;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto json = core::Json::parse(buffer.str(), nullptr, false);
    if (json.is_discarded() || !json.is_object() || json.value("version", 0) != kCacheVersion) return;
    try {
        for (const auto& item : json.at("apps")) {
            auto app = app_from_json(item);
            if (!app.path.empty()) view_.apps.push_back(std::move(app));
        }
        if (const auto it = json.find("first_seen"); it != json.end() && it->is_object()) {
            for (const auto& [key, value] : it->items()) view_.first_seen[key] = value.get<std::int64_t>();
        }
        view_.updated_at = json.value("updated_at", std::int64_t{0});
        baseline_ = false;
    } catch (const std::exception&) {
        view_ = {};
    }
    committed_ = view_.apps;
}

void AppCatalog::persist(const CatalogView& view) const {
    core::Json apps = core::Json::array();
    for (const auto& app : view.apps) apps.push_back(to_json(app));
    core::Json first = core::Json::object();
    for (const auto& [key, stamp] : view.first_seen) first[key] = stamp;
    const core::Json json{{"version", kCacheVersion},
                          {"updated_at", view.updated_at},
                          {"apps", std::move(apps)},
                          {"first_seen", std::move(first)}};
    const auto temp = std::filesystem::path(cache_file_).concat(L".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << json.dump();
    }
    std::error_code ec;
    std::filesystem::rename(temp, cache_file_, ec);
}

CatalogView AppCatalog::view() const {
    std::scoped_lock lock(mutex_);
    auto copy = view_;
    copy.scanning = scanning_.load();
    return copy;
}

bool AppCatalog::refresh(system::CatalogRequest request) {
    {
        std::scoped_lock lock(mutex_);
        if (scanning_) {
            pending_ = std::move(request);
            return false;
        }
        scanning_ = true;
    }
    cancel_ = false;
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, request = std::move(request)]() mutable { run(std::move(request)); });
    return true;
}

bool AppCatalog::refresh_if_stale(std::chrono::seconds age, const system::CatalogRequest& request) {
    {
        std::scoped_lock lock(mutex_);
        if (scanning_ || now_seconds() - view_.updated_at < age.count()) return false;
    }
    return refresh(request);
}

void AppCatalog::cancel() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void AppCatalog::run(system::CatalogRequest request) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        scan(std::move(request));
        std::scoped_lock lock(mutex_);
        if (!pending_ || cancel_) {
            pending_.reset();
            scanning_ = false;
            return;
        }
        request = std::move(*pending_);
        pending_.reset();
    }
}

void AppCatalog::scan(system::CatalogRequest request) {
    request.cancel = &cancel_;
    request.progress = [this](const std::vector<system::InstalledApp>& partial) {
        std::unordered_set<std::string> present;
        std::vector<system::InstalledApp> merged = partial;
        for (const auto& app : partial)
            present.insert(system::normalize_app_key(system::path_from_utf8(app.path)));
        std::vector<system::InstalledApp> previous;
        {
            std::scoped_lock lock(mutex_);
            previous = committed_;
        }
        for (auto& app : previous) {
            if (!present.contains(system::normalize_app_key(system::path_from_utf8(app.path))))
                merged.push_back(std::move(app));
        }
        std::scoped_lock lock(mutex_);
        view_.apps = std::move(merged);
        ++view_.generation;
    };

    auto catalog = system::discover_catalog(request);
    {
        std::scoped_lock lock(mutex_);
        if (catalog.cancelled) {
            view_.apps = committed_;
        } else {
            const auto stamp = now_seconds();
            std::unordered_set<std::string> live;
            for (const auto& app : catalog.apps) {
                const auto key = system::normalize_app_key(system::path_from_utf8(app.path));
                live.insert(key);
                view_.first_seen.try_emplace(key, baseline_ ? 0 : stamp);
            }
            std::erase_if(view_.first_seen, [&](const auto& entry) { return !live.contains(entry.first); });
            view_.apps = std::move(catalog.apps);
            view_.updated_at = stamp;
            committed_ = view_.apps;
            baseline_ = false;
            persist(view_);
        }
        view_.sources = std::move(catalog.sources);
        ++view_.generation;
    }
}

}  // namespace houseguest::service
