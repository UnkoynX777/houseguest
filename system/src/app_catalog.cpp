#include "houseguest/system/app_catalog.hpp"

// clang-format off
#include <windows.h>
#include <objbase.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "catalog_providers.hpp"

namespace houseguest::system {

using namespace catalog;

namespace {

constexpr std::size_t kMaxEntries = 8000;
constexpr std::size_t kMaxEvidence = 10;
constexpr auto kProgressInterval = std::chrono::milliseconds(750);
constexpr std::size_t kMaxCandidates = 6;
constexpr std::size_t kMaxDiscardLog = 300;
constexpr std::size_t kMaxIconSources = 6;
constexpr int kConfidentScore = 45;
constexpr int kConfidentMargin = 12;
constexpr std::int64_t kRescanAfterSeconds = 24 * 3600;

struct Entry {
    InstalledApp app;
    std::string key;
    std::string root_key;
    fs::path root;
    int name_rank = -1;
    bool user_facing = false;
    bool has_exe = false;
    std::vector<std::pair<fs::path, int>> executables;
    std::vector<FileEntry> known;
    std::string product_key;
    std::string exe_key;
};

void add_unique(std::vector<std::string>& list, const std::string& value) {
    if (std::ranges::find(list, value) == list.end()) list.push_back(value);
}

void add_evidence(Entry& entry, const std::vector<Evidence>& more) {
    for (const auto& item : more) {
        if (entry.app.evidence.size() >= kMaxEvidence) return;
        const bool known = std::ranges::any_of(entry.app.evidence, [&](const Evidence& e) {
            return e.kind == item.kind && e.detail == item.detail;
        });
        if (!known) entry.app.evidence.push_back(item);
    }
}

std::string product_key_of(const std::string& platform, const std::string& product) {
    return platform.empty() || product.empty() ? std::string{} : platform + ":" + product;
}

int kind_rank(AppKind kind) {
    switch (kind) {
        case AppKind::Manual:
            return 3;
        case AppKind::Store:
            return 2;
        case AppKind::Installed:
            return 1;
        case AppKind::Portable:
            return 0;
    }
    return 0;
}

struct Stats {
    std::map<std::string, SourceReport>* reports = nullptr;
    void found(const std::string& source) { ++(*reports)[source].found; }
    void merged(const std::string& source) { ++(*reports)[source].merged; }
};

class Builder {
public:
    void integrate(std::vector<Claim>& claims, Stats stats) {
        std::vector<Claim*> with_exe, rooted, product_only;
        for (auto& claim : claims) {
            stats.found(claim.source);
            std::ranges::sort(claim.executables,
                              [](const auto& a, const auto& b) { return a.second > b.second; });
            if (std::ranges::any_of(claim.executables, [&](const auto& item) { return usable(item.first); }))
                with_exe.push_back(&claim);
            else if (!claim.root.empty())
                rooted.push_back(&claim);
            else if (!product_key_of(claim.platform, claim.product_id).empty())
                product_only.push_back(&claim);
        }
        for (auto* claim : with_exe) place_explicit(*claim, stats);
        for (auto* claim : rooted) place_rooted(*claim, stats);
        for (auto* claim : product_only) {
            if (const auto it = by_product_.find(product_key_of(claim->platform, claim->product_id));
                it != by_product_.end()) {
                merge(entries[it->second], *claim);
                stats.merged(claim->source);
            }
        }
    }

    std::vector<Entry> entries;

private:
    void place_explicit(Claim& claim, Stats stats) {
        fs::path exe;
        for (const auto& [file, weight] : claim.executables) {
            if (usable(file)) {
                exe = file;
                break;
            }
        }
        const auto key = "exe:" + key_of(exe);
        if (const auto it = by_key_.find(key); it != by_key_.end()) {
            merge(entries[it->second], claim);
            stats.merged(claim.source);
            return;
        }
        if (entries.size() >= kMaxEntries) return;
        Entry entry;
        entry.key = key;
        entry.has_exe = true;
        entry.exe_key = key.substr(4);
        entry.app.path = path_to_utf8(exe);
        create(std::move(entry), claim, claim.root.empty() ? exe.parent_path() : claim.root);
    }

    void place_rooted(Claim& claim, Stats stats) {
        const auto root_key = key_of(claim.root);
        if (const auto product = product_key_of(claim.platform, claim.product_id); !product.empty()) {
            if (const auto it = by_product_.find(product); it != by_product_.end()) {
                merge(entries[it->second], claim);
                stats.merged(claim.source);
                return;
            }
        }
        if (const auto it = by_key_.find("root:" + root_key); it != by_key_.end()) {
            merge(entries[it->second], claim);
            stats.merged(claim.source);
            return;
        }
        std::size_t best = entries.size();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!entries[i].has_exe ||
                !key_inside(root_key, entries[i].exe_key))
                continue;
            if (best == entries.size() || entries[i].app.evidence.size() > entries[best].app.evidence.size())
                best = i;
        }
        if (best != entries.size()) {
            merge(entries[best], claim);
            stats.merged(claim.source);
            return;
        }
        if (entries.size() >= kMaxEntries) return;
        Entry entry;
        entry.key = "root:" + root_key;
        create(std::move(entry), claim, claim.root);
    }

    void create(Entry entry, Claim& claim, const fs::path& root) {
        entry.root = root;
        entry.root_key = key_of(root);
        entry.app.install_dir = path_to_utf8(root);
        entry.app.kind = claim.kind;
        index(entries.size(), entry.key, product_key_of(claim.platform, claim.product_id));
        entries.push_back(std::move(entry));
        merge(entries.back(), claim);
    }

    void index(std::size_t position, const std::string& key, const std::string& product) {
        by_key_[key] = position;
        if (!product.empty()) by_product_.try_emplace(product, position);
    }

    void merge(Entry& entry, Claim& claim) {
        add_unique(entry.app.sources, claim.source);
        entry.app.source = entry.app.sources.front();
        add_evidence(entry, claim.evidence);
        if (claim.name_rank > entry.name_rank && !claim.name.empty()) {
            entry.app.name = claim.name;
            entry.name_rank = claim.name_rank;
        }
        const auto fill = [](std::string& target, const std::string& value) {
            if (target.empty()) target = value;
        };
        fill(entry.app.company, claim.publisher);
        fill(entry.app.version, claim.version);
        fill(entry.app.scope, claim.scope);
        fill(entry.app.package_family, claim.package_family);
        fill(entry.app.platform, claim.platform);
        fill(entry.app.product_id, claim.product_id);
        if (const auto product = product_key_of(entry.app.platform, entry.app.product_id);
            !product.empty() && entry.product_key != product) {
            entry.product_key = product;
            by_product_.try_emplace(product, static_cast<std::size_t>(&entry - entries.data()));
        }
        if (kind_rank(claim.kind) > kind_rank(entry.app.kind)) entry.app.kind = claim.kind;
        entry.user_facing = entry.user_facing || claim.user_facing;
        if (!claim.root.empty()) {
            const auto root_key = key_of(claim.root);
            if (entry.root.empty() || key_inside(root_key, entry.root_key)) {
                entry.root = claim.root;
                entry.root_key = root_key;
                entry.app.install_dir = path_to_utf8(claim.root);
            }
        }
        for (auto& item : claim.executables) entry.executables.push_back(item);
        for (auto& icon : claim.icons) {
            if (entry.app.icons.size() < kMaxIconSources &&
                std::ranges::none_of(entry.app.icons, [&](const IconSource& known) {
                    return known.kind == icon.kind && known.path == icon.path && known.index == icon.index;
                }))
                entry.app.icons.push_back(std::move(icon));
        }
        if (entry.known.size() < 400) {
            for (auto& file : claim.known_executables) entry.known.push_back(file);
        }
    }

    // Resolving a path to its canonical form and checking that a file exists both touch the disk, and the same
    // few hundred paths are asked about by many claims, so each answer is looked up once per build.
    const std::string& key_of(const fs::path& path) {
        auto [it, inserted] = keys_.try_emplace(path.native());
        if (inserted) it->second = canonical_key(path);
        return it->second;
    }

    bool usable(const fs::path& file) {
        auto [it, inserted] = usable_.try_emplace(file.native(), false);
        if (inserted) it->second = usable_executable(file);
        return it->second;
    }

    std::unordered_map<std::string, std::size_t> by_key_;
    std::unordered_map<std::string, std::size_t> by_product_;
    std::unordered_map<std::wstring, std::string> keys_;
    std::unordered_map<std::wstring, bool> usable_;
};

struct ResolveContext {
    const CatalogRequest& request;
    const std::vector<std::string>& running;
    std::mutex mutex;
    std::vector<Discarded> discarded;
    std::chrono::steady_clock::time_point deadline;
};

void note_discard(ResolveContext& ctx, const fs::path& path, const std::string& reason) {
    std::scoped_lock lock(ctx.mutex);
    if (ctx.discarded.size() < kMaxDiscardLog)
        ctx.discarded.push_back({path_to_utf8(path), reason, "resolver"});
}

void apply_candidate_choice(Entry& entry, const fs::path& exe, const char* evidence) {
    entry.app.path = path_to_utf8(exe);
    entry.has_exe = true;
    entry.app.resolution = Resolution::Resolved;
    add_evidence(entry, {{evidence, path_to_utf8(exe)}});
}

void resolve_entry(Entry& entry, ResolveContext& ctx) {
    const auto name_tokens = tokens(entry.app.name);
    const auto publisher = alnum(entry.app.company);

    if (const auto chosen = ctx.request.resolutions.find(entry.root_key);
        chosen != ctx.request.resolutions.end()) {
        const auto file = path_from_utf8(chosen->second);
        if (usable_executable(file)) {
            apply_candidate_choice(entry, file, "user");
            entry.app.scanned_at = epoch_now();
            return;
        }
        if (entry.has_exe) return;
    }

    std::vector<FileEntry> executables = entry.known;
    if (executables.empty()) {
        WalkBudget budget;
        budget.cancel = ctx.request.cancel;
        executables = find_executables(entry.root, budget).executables;
    }

    std::vector<ExecutableCandidate> graded;
    std::size_t graphical = 0;
    for (const auto& file : executables) {
        const auto stem = path_to_utf8(file.path.stem());
        if (const auto role = classify_executable(file.path, {});
            role == AppRole::Installer || role == AppRole::Helper) {
            note_discard(
                ctx, file.path,
                role == AppRole::Installer ? "installer or uninstaller" : "helper or service component");
            continue;
        }
        ExecutableCandidate candidate;
        candidate.path = path_to_utf8(file.path);
        const auto subsystem = pe_subsystem(file.path);
        if (subsystem == Subsystem::Gui) {
            candidate.score += 30;
            candidate.reasons.push_back("graphical program");
            ++graphical;
        } else if (subsystem == Subsystem::Console) {
            candidate.score -= 20;
            candidate.reasons.push_back("console program");
        }
        const auto version = read_version(file.path);
        if (!version.product.empty() || !version.description.empty()) {
            candidate.score += 5;
            const auto product = token_overlap(name_tokens, tokens(version.product));
            const auto description = token_overlap(name_tokens, tokens(version.description));
            if (product > 0) {
                candidate.score += static_cast<int>(std::lround(product * 25));
                candidate.reasons.push_back("product name matches");
            }
            if (description > 0) candidate.score += static_cast<int>(std::lround(description * 10));
        }
        if (!publisher.empty() && !version.company.empty()) {
            const auto company = alnum(version.company);
            if (company.find(publisher) != std::string::npos ||
                publisher.find(company) != std::string::npos) {
                candidate.score += 10;
                candidate.reasons.push_back("same publisher");
            }
        }
        if (const auto overlap = token_overlap(name_tokens, tokens(stem)); overlap > 0)
            candidate.score += static_cast<int>(std::lround(overlap * 10));
        if (std::ranges::binary_search(ctx.running, canonical_key(file.path))) {
            candidate.score += 40;
            candidate.reasons.push_back("running now");
        }
        candidate.score +=
            std::min(10, static_cast<int>(std::log2(static_cast<double>(file.size) / 65536.0 + 1.0)));
        graded.push_back(std::move(candidate));
    }
    std::ranges::stable_sort(graded, [](const auto& a, const auto& b) { return a.score > b.score; });
    entry.app.scanned_at = epoch_now();
    if (graded.empty()) {
        entry.app.resolution = Resolution::Unresolved;
        return;
    }
    const bool running =
        std::ranges::any_of(graded.front().reasons, [](const std::string& r) { return r == "running now"; });
    const int margin = graded.size() > 1 ? graded[0].score - graded[1].score : kConfidentMargin;
    const bool surveyed = std::ranges::all_of(
        entry.app.evidence, [](const Evidence& item) { return item.kind == "volume_survey"; });
    const bool confident =
        running || (graded[0].score >= kConfidentScore && margin >= kConfidentMargin) ||
        (!surveyed &&
         (graded.size() == 1 || (graphical == 1 && graded.front().reasons.front() == "graphical program")));
    entry.app.candidates.assign(graded.begin(), graded.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                                     graded.size(), kMaxCandidates)));
    if (confident) {
        apply_candidate_choice(entry, path_from_utf8(graded.front().path), "resolved");
        entry.app.resolution = Resolution::Resolved;
    } else {
        entry.app.resolution = Resolution::Candidates;
    }
}

bool reusable(const InstalledApp& previous, std::int64_t now, bool deep) {
    if (previous.resolution == Resolution::Resolved && !previous.path.empty()) {
        std::error_code ec;
        const auto file = path_from_utf8(previous.path);
        return fs::is_regular_file(file, ec) &&
               static_cast<std::int64_t>(fs::file_size(file, ec)) == previous.size;
    }
    return !deep && previous.scanned_at != 0 && now - previous.scanned_at < kRescanAfterSeconds;
}

void finish_metadata(Entry& entry,
                     const std::unordered_map<std::string, const InstalledApp*>& previous_by_path) {
    if (entry.app.path.empty()) {
        entry.app.volume = volume_of(entry.root);
        return;
    }
    const auto file = path_from_utf8(entry.app.path);
    std::error_code ec;
    entry.app.size = static_cast<std::int64_t>(fs::file_size(file, ec));
    const auto stamp = fs::last_write_time(file, ec);
    entry.app.modified =
        ec ? 0
           : std::chrono::duration_cast<std::chrono::seconds>(
                 std::chrono::clock_cast<std::chrono::system_clock>(stamp).time_since_epoch())
                 .count();
    entry.app.volume = volume_of(file);
    const auto known = previous_by_path.find(canonical_key(file));
    if (known != previous_by_path.end() && known->second->size == entry.app.size &&
        known->second->modified == entry.app.modified && !known->second->description.empty()) {
        if (entry.app.company.empty()) entry.app.company = known->second->company;
        if (entry.app.version.empty()) entry.app.version = known->second->version;
        entry.app.description = known->second->description;
    } else {
        const auto version = read_version(file);
        if (entry.app.company.empty()) entry.app.company = version.company;
        if (entry.app.version.empty()) entry.app.version = version.version;
        entry.app.description = !version.description.empty() ? version.description : version.product;
    }
    if (entry.app.name.empty())
        entry.app.name = !entry.app.description.empty() ? entry.app.description : path_to_utf8(file.stem());
}

void classify(Entry& entry) {
    if (entry.app.resolution == Resolution::Unresolved) {
        entry.app.role = AppRole::Helper;
        return;
    }
    if (entry.app.path.empty()) {
        entry.app.role = AppRole::Application;
        return;
    }
    entry.app.role = classify_executable(path_from_utf8(entry.app.path), entry.app.name);
    if (entry.app.role == AppRole::Application && !entry.user_facing)
        entry.app.role = AppRole::Tool;
    else if (entry.app.role == AppRole::Tool && entry.user_facing)
        entry.app.role = AppRole::Application;
}

std::string drive_text(const std::string& root) {
    return root.size() >= 2 ? root.substr(0, 2) : root;
}

}  // namespace

const char* kind_name(AppKind kind) {
    switch (kind) {
        case AppKind::Installed:
            return "installed";
        case AppKind::Portable:
            return "portable";
        case AppKind::Store:
            return "store";
        case AppKind::Manual:
            return "manual";
    }
    return "installed";
}

const char* role_name(AppRole role) {
    switch (role) {
        case AppRole::Application:
            return "application";
        case AppRole::Tool:
            return "tool";
        case AppRole::Helper:
            return "helper";
        case AppRole::Installer:
            return "installer";
    }
    return "application";
}

const char* resolution_name(Resolution resolution) {
    switch (resolution) {
        case Resolution::Resolved:
            return "resolved";
        case Resolution::Candidates:
            return "candidates";
        case Resolution::Unresolved:
            return "unresolved";
    }
    return "resolved";
}

AppRole classify_executable(const fs::path& file, const std::string& name) {
    const auto stem = lower(path_to_utf8(file.stem()));
    const auto label = lower(name);
    const auto path = lower(path_to_utf8(file));
    const auto starts = [&](const char* prefix) { return stem.starts_with(prefix); };
    const auto has = [&](const char* part) { return stem.find(part) != std::string::npos; };
    const auto inside = [&](const char* part) { return path.find(part) != std::string::npos; };

    if (starts("unins") || starts("uninst") || starts("setup") || starts("install") || starts("vc_redist") ||
        starts("vcredist") || starts("dxsetup") || starts("msiexec") || stem.ends_with("setup") ||
        stem.ends_with("installer") || stem.ends_with("_uninstall") || label.starts_with("uninstall") ||
        label.starts_with("desinstal")) {
        return AppRole::Installer;
    }
    if (has("helper") || has("crashpad") || has("crashreport") || has("crashhandler") ||
        has("crash_handler") || has("errorreport") || has("elevate") || has("updater") || starts("update") ||
        stem.ends_with("update") || has("notifier") || has("broker") || has("webview") ||
        stem == "cefsharp.browsersubprocess" || stem.ends_with("_proxy") || stem.ends_with("-proxy") ||
        inside("\\driverstore\\") || inside("\\system32\\drivers\\") || stem.ends_with("svc") ||
        stem.ends_with("service")) {
        return AppRole::Helper;
    }
    if (under_windows(path) && !inside("\\systemapps\\")) return AppRole::Tool;
    return AppRole::Application;
}

std::string normalize_app_key(const fs::path& file) {
    return canonical_key(file);
}

InstalledApp describe_executable(const fs::path& file) {
    InstalledApp app;
    const auto version = read_version(file);
    app.path = path_to_utf8(file);
    app.company = version.company;
    app.version = version.version;
    app.description = !version.description.empty() ? version.description : version.product;
    app.name = !app.description.empty() ? app.description : path_to_utf8(file.stem());
    app.install_dir = path_to_utf8(file.parent_path());
    app.source = "manual";
    app.sources = {"manual"};
    app.kind = AppKind::Manual;
    app.volume = volume_of(file);
    app.id = hash_text("exe:" + canonical_key(file));
    return app;
}

std::vector<VolumeInfo> enumerate_volumes(bool include_removable) {
    std::vector<VolumeInfo> volumes;
    const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        const std::wstring root = {static_cast<wchar_t>(L'A' + i), L':', L'\\'};
        VolumeInfo info;
        info.root = to_utf8(root);
        wchar_t label[MAX_PATH] = {}, filesystem[MAX_PATH] = {};
        DWORD serial = 0;
        info.ready = GetVolumeInformationW(root.c_str(), label, MAX_PATH, &serial, nullptr, nullptr,
                                           filesystem, MAX_PATH) != 0;
        info.label = to_utf8(label);
        info.filesystem = to_utf8(filesystem);
        info.serial = serial;
        switch (GetDriveTypeW(root.c_str())) {
            case DRIVE_FIXED:
                info.type = "fixed";
                info.included = true;
                break;
            case DRIVE_RAMDISK:
                info.type = "ramdisk";
                info.included = true;
                break;
            case DRIVE_REMOVABLE:
                info.type = "removable";
                info.included = include_removable;
                if (!include_removable)
                    info.reason = "removable drives are skipped unless enabled in the library settings";
                break;
            case DRIVE_CDROM:
                info.type = "optical";
                info.reason = "optical drives are never scanned";
                break;
            case DRIVE_REMOTE:
                info.type = "network";
                info.reason = "network locations are never opened by the privileged service";
                break;
            default:
                info.type = "unknown";
                info.reason = "unknown drive type";
                break;
        }
        if (info.included && !info.ready) info.reason = "the volume is not ready";
        volumes.push_back(std::move(info));
    }
    SetErrorMode(mode);
    return volumes;
}

std::vector<SourceReport> describe_sources() {
    std::vector<SourceReport> reports;
    for (const auto& provider : providers()) {
        SourceReport report;
        report.name = provider.name;
        report.note = provider.note;
        reports.push_back(std::move(report));
    }
    return reports;
}

namespace {

Catalog build(const CatalogRequest& request, std::vector<Claim>& claims, std::vector<Discarded> discarded,
              std::map<std::string, SourceReport>& reports, const std::vector<VolumeInfo>& volumes,
              const std::vector<std::string>& running) {
    Catalog catalog;
    catalog.volumes = volumes;
    Builder builder;
    builder.integrate(claims, Stats{&reports});

    std::unordered_map<std::string, const InstalledApp*> previous_by_id, previous_by_path;
    for (const auto& previous : request.previous) {
        previous_by_id[previous.id] = &previous;
        if (!previous.path.empty())
            previous_by_path[canonical_key(path_from_utf8(previous.path))] = &previous;
    }

    const auto now = epoch_now();
    std::vector<std::size_t> pending;
    for (std::size_t i = 0; i < builder.entries.size(); ++i) {
        auto& entry = builder.entries[i];
        entry.app.id = hash_text(entry.key);
        if (!entry.has_exe) {
            const auto found = previous_by_id.find(entry.app.id);
            if (found != previous_by_id.end() && reusable(*found->second, now, request.deep_scan) &&
                !request.resolutions.contains(entry.root_key)) {
                const auto& old = *found->second;
                entry.app.path = old.path;
                entry.app.candidates = old.candidates;
                entry.app.resolution = old.resolution;
                entry.app.scanned_at = old.scanned_at;
                entry.has_exe = !old.path.empty();
                continue;
            }
            pending.push_back(i);
        } else if (const auto chosen = request.resolutions.find(entry.root_key);
                   chosen != request.resolutions.end()) {
            pending.push_back(i);
        }
    }

    ResolveContext resolve{
        request,
        running,
        {},
        {},
        std::chrono::steady_clock::now() + std::chrono::seconds(request.deep_scan ? 300 : 90)};
    std::atomic<std::size_t> next{0};
    const auto worker = [&] {
        for (;;) {
            const auto position = next.fetch_add(1);
            if (position >= pending.size()) return;
            auto& entry = builder.entries[pending[position]];
            if ((request.cancel != nullptr && request.cancel->load()) ||
                std::chrono::steady_clock::now() > resolve.deadline) {
                entry.app.resolution = Resolution::Unresolved;
                add_evidence(
                    entry, {{"pending", "the time budget ran out; it will be retried on the next refresh"}});
                continue;
            }
            resolve_entry(entry, resolve);
        }
    };
    {
        const auto count =
            std::min<std::size_t>(3, std::max<std::size_t>(1, std::thread::hardware_concurrency() / 2));
        std::vector<std::jthread> pool;
        for (std::size_t i = 1; i < count && pending.size() > 1; ++i) pool.emplace_back(worker);
        worker();
    }
    for (auto& entry : builder.entries) {
        if (entry.has_exe && entry.app.resolution != Resolution::Resolved)
            entry.app.resolution = Resolution::Resolved;
    }

    std::unordered_map<std::string, std::size_t> by_exe;
    std::vector<Entry> merged;
    for (auto& entry : builder.entries) {
        if (entry.has_exe) {
            const auto key = canonical_key(path_from_utf8(entry.app.path));
            if (const auto it = by_exe.find(key); it != by_exe.end()) {
                auto& keep = merged[it->second];
                for (const auto& source : entry.app.sources) add_unique(keep.app.sources, source);
                add_evidence(keep, entry.app.evidence);
                if (keep.app.platform.empty()) keep.app.platform = entry.app.platform;
                if (keep.app.product_id.empty()) keep.app.product_id = entry.app.product_id;
                if (entry.name_rank > keep.name_rank) {
                    keep.app.name = entry.app.name;
                    keep.name_rank = entry.name_rank;
                }
                keep.user_facing = keep.user_facing || entry.user_facing;
                continue;
            }
            by_exe[key] = merged.size();
        }
        merged.push_back(std::move(entry));
    }

    for (auto& entry : merged) {
        finish_metadata(entry, previous_by_path);
        classify(entry);
    }

    std::set<std::string> present;
    for (auto& entry : merged) {
        present.insert(entry.app.id);
        catalog.apps.push_back(std::move(entry.app));
    }
    for (const auto& previous : request.previous) {
        if (present.contains(previous.id)) continue;
        const auto volume = std::ranges::find_if(
            volumes, [&](const VolumeInfo& v) { return drive_text(v.root) == previous.volume; });
        const bool offline =
            !previous.volume.empty() && (volume == volumes.end() || !volume->ready || !volume->included);
        const bool survey = previous.source == "volume_survey" && !request.deep_scan;
        std::error_code ec;
        const bool exists =
            !previous.install_dir.empty() && fs::is_directory(path_from_utf8(previous.install_dir), ec);
        if (offline || (survey && exists)) {
            auto kept = previous;
            kept.offline = offline;
            catalog.apps.push_back(std::move(kept));
        }
    }

    std::ranges::sort(catalog.apps, [](const InstalledApp& a, const InstalledApp& b) {
        return lower(a.name) < lower(b.name);
    });
    for (const auto& app : catalog.apps) {
        if (app.sources.size() == 1) ++reports[app.sources.front()].unique;
    }
    for (auto& entry : resolve.discarded) discarded.push_back(std::move(entry));
    if (discarded.size() > kMaxDiscardLog) discarded.resize(kMaxDiscardLog);
    catalog.discarded = std::move(discarded);
    return catalog;
}

}  // namespace

Catalog discover_catalog(const CatalogRequest& request) {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const auto volumes = enumerate_volumes(request.include_removable);
    const auto running = running_images();
    std::vector<Claim> claims;
    std::vector<Discarded> discarded;
    std::map<std::string, SourceReport> reports;
    std::vector<std::string> order;
    auto last_progress = std::chrono::steady_clock::now() - kProgressInterval;

    for (const auto& provider : providers()) {
        if (provider.deep_only && !request.deep_scan) continue;
        if (request.cancel != nullptr && request.cancel->load()) break;
        ProviderContext context(request, volumes, running);
        context.source = provider.name;
        context.seed(claims);
        const auto started = std::chrono::steady_clock::now();
        try {
            provider.run(context);
        } catch (const std::exception& error) {
            context.available = false;
            context.error = error.what();
        }
        SourceReport report;
        report.name = provider.name;
        report.note = provider.note;
        report.available = context.available;
        report.error = context.error;
        if (!context.note.empty()) report.note += std::string(" (") + context.note + ")";
        report.discarded = context.discard_count;
        report.milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
                .count();
        reports[provider.name] = std::move(report);
        order.push_back(provider.name);
        for (auto& claim : context.claims) claims.push_back(std::move(claim));
        for (auto& entry : context.discarded) discarded.push_back(std::move(entry));
        if (request.progress && std::chrono::steady_clock::now() - last_progress >= kProgressInterval) {
            last_progress = std::chrono::steady_clock::now();
            std::vector<Claim> provisional = claims;
            std::map<std::string, SourceReport> scratch;
            Builder builder;
            builder.integrate(provisional, Stats{&scratch});
            std::vector<InstalledApp> preview;
            for (auto& entry : builder.entries) {
                if (!entry.has_exe) continue;
                entry.app.id = hash_text(entry.key);
                entry.app.role = entry.user_facing ? AppRole::Application : AppRole::Tool;
                preview.push_back(std::move(entry.app));
            }
            request.progress(preview);
        }
    }

    auto catalog = build(request, claims, std::move(discarded), reports, volumes, running);
    for (const auto& name : order) catalog.sources.push_back(reports[name]);
    catalog.cancelled = request.cancel != nullptr && request.cancel->load();
    if (initialized) CoUninitialize();
    return catalog;
}

std::vector<InstalledApp> scan_folder(const fs::path& folder, const std::atomic<bool>* cancel) {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    CatalogRequest request;
    request.cancel = cancel;
    std::vector<Claim> claims = group_installations(folder, "folder", cancel);
    std::map<std::string, SourceReport> reports;
    const auto running = running_images();
    auto catalog = build(request, claims, {}, reports, {}, running);
    if (initialized) CoUninitialize();
    return std::move(catalog.apps);
}

}  // namespace houseguest::system
