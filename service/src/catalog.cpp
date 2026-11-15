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

constexpr int kCacheVersion = 3;

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

system::Resolution resolution_from(const std::string& name) {
    if (name == "candidates") return system::Resolution::Candidates;
    if (name == "unresolved") return system::Resolution::Unresolved;
    return system::Resolution::Resolved;
}

std::string volume_signature(bool include_removable) {
    std::string text;
    for (const auto& volume : system::enumerate_volumes(include_removable)) {
        text += volume.root + volume.type + (volume.ready ? "+" : "-") + std::to_string(volume.serial) + ";";
    }
    return text;
}

}  // namespace

core::Json to_json(const system::InstalledApp& app) {
    core::Json candidates = core::Json::array();
    for (const auto& candidate : app.candidates)
        candidates.push_back(
            {{"path", candidate.path}, {"score", candidate.score}, {"reasons", candidate.reasons}});
    core::Json evidence = core::Json::array();
    for (const auto& item : app.evidence) evidence.push_back({{"kind", item.kind}, {"detail", item.detail}});
    core::Json icons = core::Json::array();
    for (const auto& item : app.icons)
        icons.push_back({{"kind", item.kind}, {"path", item.path}, {"index", item.index}});
    core::Json alternatives = core::Json::array();
    for (const auto& item : app.alternatives)
        alternatives.push_back({{"field", item.field}, {"value", item.value}, {"source", item.source}});
    return {{"icons", std::move(icons)},
            {"alternatives", std::move(alternatives)},
            {"origins",
             {{"name", app.name_source}, {"company", app.company_source}, {"version", app.version_source}}},
            {"id", app.id},
            {"name", app.name},
            {"path", app.path},
            {"company", app.company},
            {"version", app.version},
            {"description", app.description},
            {"source", app.source},
            {"sources", app.sources},
            {"package_family", app.package_family},
            {"install_dir", app.install_dir},
            {"platform", app.platform},
            {"product_id", app.product_id},
            {"volume", app.volume},
            {"kind", system::kind_name(app.kind)},
            {"role", system::role_name(app.role)},
            {"resolution", system::resolution_name(app.resolution)},
            {"candidates", std::move(candidates)},
            {"evidence", std::move(evidence)},
            {"scope", app.scope},
            {"modified", app.modified},
            {"size", app.size},
            {"scanned_at", app.scanned_at},
            {"offline", app.offline}};
}

system::InstalledApp app_from_json(const core::Json& json) {
    system::InstalledApp app;
    app.id = json.value("id", "");
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
    app.platform = json.value("platform", "");
    app.product_id = json.value("product_id", "");
    app.volume = json.value("volume", "");
    app.kind = kind_from(json.value("kind", ""));
    app.role = role_from(json.value("role", ""));
    app.resolution = resolution_from(json.value("resolution", ""));
    if (const auto it = json.find("candidates"); it != json.end() && it->is_array()) {
        for (const auto& item : *it) {
            system::ExecutableCandidate candidate;
            candidate.path = item.value("path", "");
            candidate.score = item.value("score", 0);
            if (const auto reasons = item.find("reasons"); reasons != item.end() && reasons->is_array()) {
                for (const auto& reason : *reasons) {
                    if (reason.is_string()) candidate.reasons.push_back(reason.get<std::string>());
                }
            }
            app.candidates.push_back(std::move(candidate));
        }
    }
    if (const auto it = json.find("origins"); it != json.end() && it->is_object()) {
        app.name_source = it->value("name", "");
        app.company_source = it->value("company", "");
        app.version_source = it->value("version", "");
    }
    if (const auto it = json.find("alternatives"); it != json.end() && it->is_array()) {
        for (const auto& item : *it)
            app.alternatives.push_back(
                {item.value("field", ""), item.value("value", ""), item.value("source", "")});
    }
    if (const auto it = json.find("icons"); it != json.end() && it->is_array()) {
        for (const auto& item : *it)
            app.icons.push_back({item.value("kind", ""), item.value("path", ""), item.value("index", 0)});
    }
    if (const auto it = json.find("evidence"); it != json.end() && it->is_array()) {
        for (const auto& item : *it)
            app.evidence.push_back({item.value("kind", ""), item.value("detail", "")});
    }
    app.scope = json.value("scope", "");
    app.modified = json.value("modified", std::int64_t{0});
    app.size = json.value("size", std::int64_t{0});
    app.scanned_at = json.value("scanned_at", std::int64_t{0});
    app.offline = json.value("offline", false);
    return app;
}

core::Json to_json(const system::VolumeInfo& volume) {
    return {{"root", volume.root},         {"type", volume.type},
            {"label", volume.label},       {"filesystem", volume.filesystem},
            {"serial", volume.serial},     {"ready", volume.ready},
            {"included", volume.included}, {"reason", volume.reason}};
}

core::Json to_json(const system::SourceReport& report) {
    return {{"name", report.name},           {"note", report.note},
            {"available", report.available}, {"found", report.found},
            {"unique", report.unique},       {"merged", report.merged},
            {"discarded", report.discarded}, {"milliseconds", report.milliseconds},
            {"error", report.error}};
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
            if (!app.id.empty()) view_.apps.push_back(std::move(app));
        }
        if (const auto it = json.find("first_seen"); it != json.end() && it->is_object()) {
            for (const auto& [key, value] : it->items()) view_.first_seen[key] = value.get<std::int64_t>();
        }
        if (const auto it = json.find("sources"); it != json.end() && it->is_array()) {
            for (const auto& item : *it) {
                system::SourceReport report;
                report.name = item.value("name", "");
                report.note = item.value("note", "");
                report.available = item.value("available", true);
                report.found = item.value("found", std::size_t{0});
                report.unique = item.value("unique", std::size_t{0});
                report.merged = item.value("merged", std::size_t{0});
                report.discarded = item.value("discarded", std::size_t{0});
                report.milliseconds = item.value("milliseconds", std::int64_t{0});
                report.error = item.value("error", "");
                view_.sources.push_back(std::move(report));
            }
        }
        if (const auto it = json.find("discarded"); it != json.end() && it->is_array()) {
            for (const auto& item : *it)
                view_.discarded.push_back(
                    {item.value("path", ""), item.value("reason", ""), item.value("source", "")});
        }
        view_.updated_at = json.value("updated_at", std::int64_t{0});
        volume_signature_ = json.value("volumes", "");
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
    core::Json sources = core::Json::array();
    for (const auto& report : view.sources) sources.push_back(to_json(report));
    core::Json discarded = core::Json::array();
    for (const auto& item : view.discarded)
        discarded.push_back({{"path", item.path}, {"reason", item.reason}, {"source", item.source}});
    const core::Json json{{"version", kCacheVersion},         {"updated_at", view.updated_at},
                          {"volumes", volume_signature_},     {"apps", std::move(apps)},
                          {"first_seen", std::move(first)},   {"sources", std::move(sources)},
                          {"discarded", std::move(discarded)}};
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

bool AppCatalog::volumes_changed(bool include_removable) const {
    const auto current = volume_signature(include_removable);
    std::scoped_lock lock(mutex_);
    return !volume_signature_.empty() && current != volume_signature_;
}

bool AppCatalog::refresh(system::CatalogRequest request) {
    {
        std::scoped_lock lock(mutex_);
        if (scanning_) {
            pending_ = std::move(request);
            return false;
        }
        scanning_ = true;
        view_.deep = request.deep_scan;
    }
    cancel_ = false;
    if (worker_.joinable()) worker_.join();
    worker_ = std::jthread([this, request = std::move(request)]() mutable { run(std::move(request)); });
    return true;
}

bool AppCatalog::refresh_if_stale(std::chrono::seconds age, const system::CatalogRequest& request) {
    {
        std::scoped_lock lock(mutex_);
        if (scanning_) return false;
    }
    const bool changed = volumes_changed(request.include_removable);
    {
        std::scoped_lock lock(mutex_);
        if (!changed && now_seconds() - view_.updated_at < age.count()) return false;
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
            view_.deep = false;
            return;
        }
        request = std::move(*pending_);
        pending_.reset();
    }
}

void AppCatalog::scan(system::CatalogRequest request) {
    request.cancel = &cancel_;
    {
        std::scoped_lock lock(mutex_);
        request.previous = committed_;
    }
    request.progress = [this](const std::vector<system::InstalledApp>& partial) {
        std::unordered_set<std::string> present;
        std::vector<system::InstalledApp> merged = partial;
        for (const auto& app : partial) present.insert(app.id);
        std::scoped_lock lock(mutex_);
        for (const auto& app : committed_) {
            if (!present.contains(app.id)) merged.push_back(app);
        }
        view_.apps = std::move(merged);
        ++view_.generation;
    };
    const auto signature = volume_signature(request.include_removable);

    const auto started = std::chrono::steady_clock::now();
    auto catalog = system::discover_catalog(request);
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::scoped_lock lock(mutex_);
    if (catalog.cancelled) {
        view_.apps = committed_;
    } else {
        const auto stamp = now_seconds();
        std::unordered_set<std::string> live;
        for (const auto& app : catalog.apps) {
            live.insert(app.id);
            view_.first_seen.try_emplace(app.id, baseline_ ? 0 : stamp);
        }
        std::erase_if(view_.first_seen, [&](const auto& entry) { return !live.contains(entry.first); });
        view_.apps = std::move(catalog.apps);
        view_.updated_at = stamp;
        view_.scan_ms = elapsed.count();
        view_.sources = std::move(catalog.sources);
        view_.volumes = std::move(catalog.volumes);
        view_.discarded = std::move(catalog.discarded);
        committed_ = view_.apps;
        volume_signature_ = signature;
        baseline_ = false;
        persist(view_);
    }
    ++view_.generation;
}

}  // namespace houseguest::service
