#include "catalog_providers.hpp"

// clang-format off
#include <windows.h>
#include <shlwapi.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "houseguest/core/serialization.hpp"
#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::system::catalog {

namespace {

constexpr std::size_t kMaxDiscardLog = 300;

struct Profile {
    fs::path directory;
    std::string name;
};

std::vector<Profile> user_profiles() {
    std::vector<Profile> profiles;
    wchar_t drive[MAX_PATH] = {};
    GetEnvironmentVariableW(L"SystemDrive", drive, MAX_PATH);
    std::error_code ec;
    static const std::set<std::string> skipped = {"default", "default user", "all users", "public"};
    for (const auto& entry : fs::directory_iterator(fs::path(drive) / L"\\Users",
                                                    fs::directory_options::skip_permission_denied, ec)) {
        std::error_code type_error;
        if (!entry.is_directory(type_error)) continue;
        const auto name = path_to_utf8(entry.path().filename());
        if (!skipped.contains(lower(name))) profiles.push_back({entry.path(), name});
    }
    return profiles;
}

std::vector<std::wstring> loaded_user_hives() {
    std::vector<std::wstring> hives;
    for (const auto& name : subkeys(HKEY_USERS, L"")) {
        if (name.starts_with(L"S-1-5-21-") && !name.ends_with(L"_Classes")) hives.push_back(name);
    }
    return hives;
}

std::string read_text(const fs::path& file, std::size_t limit = std::size_t{2} << 20) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    if (text.size() > limit) text.clear();
    return text;
}

std::string xml_attribute(const std::string& tag, const std::string& attribute) {
    const auto at = tag.find(" " + attribute + "=\"");
    if (at == std::string::npos) return {};
    const auto begin = at + attribute.size() + 3;
    const auto end = tag.find('"', begin);
    return end == std::string::npos ? std::string{} : tag.substr(begin, end - begin);
}

std::string xml_element(const std::string& xml, const std::string& element) {
    const auto open = xml.find("<" + element + ">");
    if (open == std::string::npos) return {};
    const auto begin = open + element.size() + 2;
    const auto end = xml.find("</" + element + ">", begin);
    return end == std::string::npos ? std::string{} : xml.substr(begin, end - begin);
}

std::string tag_at(const std::string& xml, std::size_t at) {
    const auto end = xml.find('>', at);
    return end == std::string::npos ? std::string{} : xml.substr(at, end - at);
}

std::string humanize(std::string identity) {
    if (const auto dot = identity.rfind('.'); dot != std::string::npos) identity = identity.substr(dot + 1);
    std::string out;
    for (std::size_t i = 0; i < identity.size(); ++i) {
        if (i > 0 && std::isupper(static_cast<unsigned char>(identity[i])) &&
            !std::isupper(static_cast<unsigned char>(identity[i - 1])))
            out += ' ';
        out += identity[i];
    }
    return out;
}

std::string resolve_resource(const std::wstring& full_name, const std::string& package,
                             const std::string& display) {
    if (!display.starts_with("ms-resource:")) return display;
    const auto resource = L"@{" + full_name + L"? ms-resource://" + to_wide(package) + L"/resources/" +
                          to_wide(display.substr(12)) + L"}";
    wchar_t resolved[256] = {};
    if (SUCCEEDED(SHLoadIndirectString(resource.c_str(), resolved, 256, nullptr)) && resolved[0] != 0)
        return to_utf8(resolved);
    return {};
}

std::vector<std::string> quoted_tokens(const std::string& text) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '"') continue;
        std::string value;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            value += text[i];
        }
        out.push_back(std::move(value));
    }
    return out;
}

std::string vdf_value(const std::vector<std::string>& tokens, const std::string& key) {
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (_stricmp(tokens[i].c_str(), key.c_str()) == 0) return tokens[i + 1];
    }
    return {};
}

bool skip_shortcut_name(const std::string& lowered) {
    static const char* const noise[] = {"uninstall",     "desinstal", "remove ", "readme", "documentation",
                                        "release notes", "license",   "website", "help"};
    return std::ranges::any_of(noise,
                               [&](const char* word) { return lowered.find(word) != std::string::npos; });
}

bool skip_survey_directory(const std::wstring& name) {
    static const std::set<std::wstring> skipped = {
        L"node_modules", L".git",        L"windows",      L"$recycle.bin",  L"system volume information",
        L"winsxs",       L"programdata", L"appdata",      L"program files", L"program files (x86)",
        L"recovery",     L"windowsapps", L"$windows.~bt", L"windows.old",   L"__pycache__",
        L".venv",        L"venv",        L"target",       L".cargo",        L".rustup",
        L".gradle",      L".m2"};
    auto lowered = name;
    std::ranges::transform(lowered, lowered.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return skipped.contains(lowered) || lowered.starts_with(L"$");
}

struct ShortcutJob {
    fs::path link;
    const char* scope = "";
    fs::path target;
    std::string address;
    std::optional<IconLocation> icon;
};

void parse_shortcut(ShortcutJob& job) {
    if (lower(job.link.extension().string()) == ".url") {
        std::istringstream lines(read_text(job.link, 64 * 1024));
        for (std::string line; std::getline(lines, line);) {
            if (line.starts_with("URL=")) job.address = line.substr(4);
        }
        while (!job.address.empty() && (job.address.back() == '\r' || job.address.back() == ' '))
            job.address.pop_back();
        return;
    }
    auto shortcut = read_shortcut(job.link);
    job.target = std::move(shortcut.target);
    job.icon = std::move(shortcut.icon);
}

void claim_shortcut(ProviderContext& ctx, const ShortcutJob& job) {
    const auto stem = path_to_utf8(job.link.stem());
    Claim claim;
    claim.name = stem;
    claim.name_rank = 3;
    claim.scope = job.scope;
    claim.evidence.push_back({"shortcut", path_to_utf8(job.link)});
    if (lower(job.link.extension().string()) == ".url") {
        if (job.address.starts_with("steam://rungameid/")) {
            claim.platform = "steam";
            claim.product_id = job.address.substr(18);
        } else if (job.address.starts_with("com.epicgames.launcher://apps/")) {
            claim.platform = "epic";
            auto id = job.address.substr(30);
            id = id.substr(0, id.find('?'));
            claim.product_id = id.substr(id.rfind("%3A") == std::string::npos ? 0 : id.rfind("%3A") + 3);
        } else {
            ctx.discard(job.link, "web link, not a program");
            return;
        }
        ctx.claim(std::move(claim));
        return;
    }
    if (job.target.empty()) {
        ctx.discard(job.link, "shortcut has no file target");
        return;
    }
    if (!usable_executable(job.target)) {
        ctx.discard(job.link, lower(job.target.extension().string()) == ".exe"
                                  ? "target is missing or not on a local disk"
                                  : "target is not a program");
        return;
    }
    claim.root = job.target.parent_path();
    claim.executables.emplace_back(job.target, 70);
    if (job.icon) {
        std::error_code icon_error;
        if (fs::is_regular_file(job.icon->file, icon_error))
            claim.icons.push_back({"shortcut", path_to_utf8(job.icon->file), job.icon->index});
    }
    ctx.claim(std::move(claim));
}

void collect_shortcuts(ProviderContext& ctx, std::vector<ShortcutJob>& jobs, const fs::path& root,
                       const char* scope, bool recursive = true) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return;
    const auto visit = [&](const fs::directory_entry& entry) {
        std::error_code type_error;
        if (!entry.is_regular_file(type_error)) return;
        const auto extension = lower(entry.path().extension().string());
        if (extension != ".lnk" && extension != ".url") return;
        if (skip_shortcut_name(lower(path_to_utf8(entry.path().stem())))) {
            ctx.discard(entry.path(), "uninstaller, help or documentation shortcut");
            return;
        }
        jobs.push_back({entry.path(), scope, {}, {}});
    };
    if (!recursive) {
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (ctx.cancelled()) return;
            visit(*it);
        }
        return;
    }
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (ctx.cancelled()) return;
        visit(*it);
    }
}

void provide_shortcuts(ProviderContext& ctx) {
    std::vector<ShortcutJob> jobs;
    const auto common = environment_path(L"ProgramData");
    if (!common.empty())
        collect_shortcuts(ctx, jobs, common / L"Microsoft\\Windows\\Start Menu\\Programs", "machine");
    const auto pub = environment_path(L"PUBLIC");
    if (!pub.empty()) collect_shortcuts(ctx, jobs, pub / L"Desktop", "machine", false);
    for (const auto& profile : user_profiles()) {
        collect_shortcuts(ctx, jobs,
                          profile.directory / L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs",
                          "user");
        collect_shortcuts(ctx, jobs, profile.directory / L"Desktop", "user", false);
    }
    std::atomic<std::size_t> next{0};
    const auto worker = [&] {
        const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        for (auto position = next.fetch_add(1); position < jobs.size() && !ctx.cancelled();
             position = next.fetch_add(1))
            parse_shortcut(jobs[position]);
        if (initialized) CoUninitialize();
    };
    {
        std::vector<std::jthread> pool;
        for (int i = 0; i < 3 && jobs.size() > 8; ++i) pool.emplace_back(worker);
        worker();
    }
    for (const auto& job : jobs) claim_shortcut(ctx, job);
}

struct InstallFolder {
    fs::path path;
    std::vector<std::string> words;
};

std::vector<InstallFolder> install_containers(const ProviderContext& ctx) {
    std::vector<fs::path> containers;
    for (const auto* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
        const auto path = environment_path(variable);
        if (!path.empty() && std::ranges::find(containers, path) == containers.end())
            containers.push_back(path);
    }
    for (const auto& volume : ctx.volumes()) {
        if (!volume.included || !volume.ready) continue;
        for (const auto* name : {L"Program Files", L"Program Files (x86)"}) {
            const auto path = fs::path(to_wide(volume.root)) / name;
            if (std::ranges::find(containers, path) == containers.end()) containers.push_back(path);
        }
    }
    for (const auto& profile : user_profiles())
        containers.push_back(profile.directory / L"AppData\\Local\\Programs");
    std::vector<InstallFolder> folders;
    std::error_code ec;
    for (const auto& container : containers) {
        for (fs::directory_iterator it(container, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            std::error_code type_error;
            if (it->is_directory(type_error))
                folders.push_back({it->path(), tokens(path_to_utf8(it->path().filename()))});
        }
    }
    return folders;
}

fs::path correlate_install_folder(const std::vector<InstallFolder>& folders, const std::string& display) {
    const auto wanted = tokens(display);
    fs::path best;
    double best_score = 0;
    for (const auto& folder : folders) {
        if (folder.words.empty()) continue;
        const auto forward = token_overlap(wanted, folder.words);
        const auto backward = token_overlap(folder.words, wanted);
        const auto score = std::min(forward, backward);
        if (score >= 0.99 && score > best_score) {
            best = folder.path;
            best_score = score;
        }
    }
    return best;
}

void claim_uninstall(ProviderContext& ctx, const std::vector<InstallFolder>& folders, HKEY root,
                     const std::wstring& parent, REGSAM view, const char* scope) {
    for (const auto& name : subkeys(root, parent, view)) {
        if (ctx.cancelled()) return;
        const auto key = parent + L"\\" + name;
        if (registry_number(root, key, L"SystemComponent", view) != 0 ||
            !registry_text(root, key, L"ParentKeyName", view).empty())
            continue;
        const auto display = registry_text(root, key, L"DisplayName", view);
        if (display.empty()) continue;

        Claim claim;
        claim.source = "windows_registry";
        claim.name = to_utf8(display);
        while (!claim.name.empty() && claim.name.back() == ' ') claim.name.pop_back();
        claim.name_rank = 4;
        claim.scope = scope;
        claim.publisher = to_utf8(registry_text(root, key, L"Publisher", view));
        claim.version = to_utf8(registry_text(root, key, L"DisplayVersion", view));
        claim.evidence.push_back({"registry.uninstall", to_utf8(key)});

        if (const auto icon = parse_icon_location(registry_text(root, key, L"DisplayIcon", view))) {
            std::error_code icon_error;
            if (fs::is_regular_file(icon->file, icon_error))
                claim.icons.push_back({"registry", path_to_utf8(icon->file), icon->index});
            const auto& candidate = icon->file;
            if (usable_executable(candidate) && classify_executable(candidate, {}) != AppRole::Installer &&
                lower(candidate.string()).find("\\windows\\installer\\") == std::string::npos &&
                lower(candidate.string()).find("\\package cache\\") == std::string::npos) {
                claim.executables.emplace_back(candidate, 60);
                claim.root = candidate.parent_path();
            }
        }
        const auto location = expand(registry_text(root, key, L"InstallLocation", view));
        std::error_code ec;
        if (!location.empty() && is_local_disk_path(location) && fs::is_directory(location, ec))
            claim.root = location;
        if (claim.root.empty()) {
            auto uninstall = registry_text(root, key, L"UninstallString", view);
            if (!uninstall.empty() && uninstall.front() == L'"')
                uninstall = uninstall.substr(1, uninstall.find(L'"', 1) - 1);
            const auto folder = expand(uninstall).parent_path();
            if (!folder.empty() && is_local_disk_path(folder) && fs::is_directory(folder, ec) &&
                !under_windows(lower(folder.string()) + "\\") &&
                lower(folder.string()).find("\\package cache\\") == std::string::npos)
                claim.root = folder;
        }
        if (claim.root.empty()) {
            claim.root = correlate_install_folder(folders, claim.name);
            if (!claim.root.empty())
                claim.evidence.push_back({"registry.correlated", path_to_utf8(claim.root)});
        }
        if (claim.root.empty()) {
            ctx.discard(fs::path(to_utf8(key)), "uninstall entry names no install folder or program");
            continue;
        }
        ctx.claim(std::move(claim));
    }
}

void provide_registry(ProviderContext& ctx) {
    const std::wstring uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    const auto folders = install_containers(ctx);
    claim_uninstall(ctx, folders, HKEY_LOCAL_MACHINE, uninstall, KEY_WOW64_64KEY, "machine");
    claim_uninstall(ctx, folders, HKEY_LOCAL_MACHINE, uninstall, KEY_WOW64_32KEY, "machine");
    for (const auto& hive : loaded_user_hives()) {
        claim_uninstall(ctx, folders, HKEY_USERS, hive + L"\\" + uninstall, 0, "user");
        claim_uninstall(ctx, folders, HKEY_USERS,
                        hive + L"\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                        "user");
    }
}

void claim_app_paths(ProviderContext& ctx, HKEY root, const std::wstring& parent, REGSAM view,
                     const char* scope) {
    for (const auto& name : subkeys(root, parent, view)) {
        const auto value = registry_text(root, parent + L"\\" + name, nullptr, view);
        if (value.empty()) continue;
        const auto target = expand(value);
        if (!usable_executable(target)) {
            ctx.discard(target, "App Paths entry points to a missing or non-program file");
            continue;
        }
        Claim claim;
        claim.source = "app_paths";
        claim.name = path_to_utf8(target.stem());
        claim.name_rank = 2;
        claim.scope = scope;
        claim.root = target.parent_path();
        claim.executables.emplace_back(target, 50);
        claim.evidence.push_back({"registry.app_paths", to_utf8(parent + L"\\" + name)});
        ctx.claim(std::move(claim));
    }
}

void provide_app_paths(ProviderContext& ctx) {
    const std::wstring parent = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    claim_app_paths(ctx, HKEY_LOCAL_MACHINE, parent, KEY_WOW64_64KEY, "machine");
    claim_app_paths(ctx, HKEY_LOCAL_MACHINE, parent, KEY_WOW64_32KEY, "machine");
    for (const auto& hive : loaded_user_hives())
        claim_app_paths(ctx, HKEY_USERS, hive + L"\\" + parent, 0, "user");
}

struct PackageFolder {
    std::wstring full_name;
    std::string name;
    std::string publisher;
    std::vector<unsigned> version;
    fs::path location;
};

std::vector<unsigned> parse_version(const std::string& text) {
    std::vector<unsigned> parts;
    std::stringstream stream(text);
    std::string piece;
    while (std::getline(stream, piece, '.'))
        parts.push_back(static_cast<unsigned>(std::strtoul(piece.c_str(), nullptr, 10)));
    return parts;
}

// Packaged apps ship each logo as several files that differ by scale, target size and backplate ("Logo.scale-200.png",
// "Logo.altform-unplated_targetsize-256.png"). The manifest names only the base, so the largest variant that exists
// is chosen, preferring the unplated ones: they have no coloured tile behind the glyph and fill their canvas.
fs::path best_package_logo(const fs::path& package_root, const std::string& relative) {
    if (relative.empty() || relative.find("..") != std::string::npos) return {};
    const auto base = package_root / path_from_utf8(relative);
    const auto folder = base.parent_path();
    const auto stem = lower(path_to_utf8(base.stem()));
    std::error_code ec;
    fs::path best;
    int best_size = -1;
    int scanned = 0;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec && scanned < 400; it.increment(ec), ++scanned) {
        const auto name = lower(path_to_utf8(it->path().filename()));
        if (!name.starts_with(stem) || !name.ends_with(".png")) continue;
        int size = 1;
        if (const auto at = name.find("targetsize-"); at != std::string::npos)
            size = std::atoi(name.c_str() + at + 11) * 4;
        else if (const auto scale = name.find("scale-"); scale != std::string::npos)
            size = std::atoi(name.c_str() + scale + 6);
        if (name.find("altform-lightunplated") != std::string::npos || name.find("contrast-") != std::string::npos)
            size = 0;
        else if (name.find("altform-unplated") != std::string::npos)
            size += 100000;
        if (size > best_size) {
            best_size = size;
            best = it->path();
        }
    }
    return best;
}

void provide_packages(ProviderContext& ctx) {
    const auto root = environment_path(L"ProgramFiles") / L"WindowsApps";
    std::error_code ec;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        ctx.available = false;
        ctx.error = "the WindowsApps folder cannot be listed: " + ec.message();
        return;
    }
    std::map<std::string, PackageFolder> newest;
    for (const fs::directory_iterator end; it != end && !ec; it.increment(ec)) {
        std::error_code type_error;
        if (!it->is_directory(type_error)) continue;
        const auto folder = path_to_utf8(it->path().filename());
        std::vector<std::string> parts;
        std::stringstream stream(folder);
        std::string piece;
        while (std::getline(stream, piece, '_')) parts.push_back(piece);
        if (!folder.empty() && folder.back() == '_') parts.emplace_back();
        if (parts.size() < 5 || !parts[3].empty()) continue;
        PackageFolder package{it->path().filename().wstring(), parts[0], parts[4], parse_version(parts[1]),
                              it->path()};
        const auto family = package.name + "_" + package.publisher;
        const auto known = newest.find(family);
        if (known == newest.end() || known->second.version < package.version)
            newest[family] = std::move(package);
    }
    for (const auto& [family, package] : newest) {
        if (ctx.cancelled()) return;
        if (lower(package.location.string()).find("\\systemapps\\") != std::string::npos) continue;
        const auto manifest = read_text(package.location / L"AppxManifest.xml", 1u << 20);
        if (manifest.empty() || xml_element(manifest, "Framework") == "true") continue;
        const auto identity_at = manifest.find("<Identity ");
        const auto identity =
            identity_at == std::string::npos ? std::string{} : tag_at(manifest, identity_at);
        const auto package_name = xml_attribute(identity, "Name");
        const auto package_display =
            resolve_resource(package.full_name, package_name, xml_element(manifest, "DisplayName"));
        auto publisher = xml_element(manifest, "PublisherDisplayName");
        if (publisher.starts_with("ms-resource:")) publisher.clear();

        for (auto at = manifest.find("<Application "); at != std::string::npos;
             at = manifest.find("<Application ", at + 1)) {
            const auto application = tag_at(manifest, at);
            const auto executable = xml_attribute(application, "Executable");
            if (executable.empty() || lower(executable).find(".exe") == std::string::npos) continue;
            const auto block_end = manifest.find("</Application>", at);
            const auto block =
                manifest.substr(at, block_end == std::string::npos ? std::string::npos : block_end - at);
            const auto visual_at = block.find("VisualElements ");
            const auto visual =
                visual_at == std::string::npos ? std::string{} : " " + tag_at(block, visual_at);
            if (xml_attribute(visual, "AppListEntry") == "none") {
                ctx.discard(package.location / path_from_utf8(executable),
                            "packaged app is hidden from the app list");
                continue;
            }
            const auto target = package.location / path_from_utf8(executable);
            if (!usable_executable(target)) {
                ctx.discard(target, "packaged executable is not readable");
                continue;
            }
            auto display =
                resolve_resource(package.full_name, package_name, xml_attribute(visual, "DisplayName"));
            if (display.empty()) display = package_display;
            if (display.empty()) display = humanize(package_name);
            Claim claim;
            claim.source = "packages";
            claim.platform = "store";
            claim.kind = AppKind::Store;
            claim.name = display;
            claim.name_rank = 5;
            claim.publisher = publisher;
            claim.scope = "machine";
            claim.package_family = package.name + "_" + package.publisher;
            claim.product_id = claim.package_family + "!" + xml_attribute(application, "Id");
            claim.root = package.location;
            claim.executables.emplace_back(target, 90);
            for (const char* attribute : {"Square44x44Logo", "Square150x150Logo"}) {
                if (const auto logo = best_package_logo(package.location, xml_attribute(visual, attribute));
                    !logo.empty()) {
                    claim.icons.push_back({"package", path_to_utf8(logo), 0});
                    break;
                }
            }
            claim.evidence.push_back(
                {"package.manifest", path_to_utf8(package.location / L"AppxManifest.xml")});
            ctx.claim(std::move(claim));
        }
    }
}

void provide_xbox(ProviderContext& ctx) {
    for (const auto& volume : ctx.volumes()) {
        if (!volume.included || !volume.ready) continue;
        const fs::path games = fs::path(to_wide(volume.root)) / L"XboxGames";
        std::error_code ec;
        for (fs::directory_iterator it(games, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (ctx.cancelled()) return;
            const auto content = it->path() / L"Content";
            const auto config = read_text(content / L"MicrosoftGame.Config", 1u << 20);
            if (config.empty()) continue;
            const auto executable_at = config.find("<Executable ");
            const auto executable = executable_at == std::string::npos
                                        ? std::string{}
                                        : xml_attribute(tag_at(config, executable_at), "Name");
            const auto identity_at = config.find("<Identity ");
            const auto identity = identity_at == std::string::npos
                                      ? std::string{}
                                      : xml_attribute(tag_at(config, identity_at), "Name");
            const auto visuals_at = config.find("<ShellVisuals ");
            auto display = visuals_at == std::string::npos
                               ? std::string{}
                               : xml_attribute(tag_at(config, visuals_at), "DefaultDisplayName");
            if (display.empty() || display.starts_with("ms-resource:"))
                display = path_to_utf8(it->path().filename());
            Claim claim;
            claim.source = "xbox_games";
            claim.platform = "xbox";
            claim.kind = AppKind::Store;
            claim.name = display;
            claim.name_rank = 6;
            claim.product_id = identity;
            claim.scope = "machine";
            claim.root = content;
            if (!executable.empty()) claim.executables.emplace_back(content / path_from_utf8(executable), 90);
            if (visuals_at != std::string::npos) {
                const auto visuals = " " + tag_at(config, visuals_at);
                for (const char* attribute : {"Square44x44Logo", "Square150x150Logo", "StoreLogo"}) {
                    if (const auto logo = best_package_logo(content, xml_attribute(visuals, attribute)); !logo.empty()) {
                        claim.icons.push_back({"package", path_to_utf8(logo), 0});
                        break;
                    }
                }
            }
            claim.evidence.push_back({"xbox.config", path_to_utf8(content / L"MicrosoftGame.Config")});
            ctx.claim(std::move(claim));
        }
    }
}

std::vector<fs::path> steam_installs() {
    std::vector<fs::path> found;
    const auto add = [&](const std::wstring& text) {
        if (text.empty()) return;
        auto path = fs::path(text).lexically_normal();
        std::error_code ec;
        if (fs::is_directory(path / L"steamapps", ec) && std::ranges::find(found, path) == found.end())
            found.push_back(path);
    };
    add(registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    add(registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"));
    add(registry_text(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    for (const auto& hive : loaded_user_hives())
        add(registry_text(HKEY_USERS, hive + L"\\Software\\Valve\\Steam", L"SteamPath"));
    return found;
}

struct KeyValue {
    std::map<std::string, KeyValue> children;
    std::string text;
};

class BinaryReader {
public:
    BinaryReader(const std::vector<std::uint8_t>& data, const std::vector<std::string>& strings)
        : data_(data), strings_(strings) {}

    bool ok() const { return !failed_; }

    void read_children(KeyValue& into, int depth) {
        while (!failed_ && depth < 16) {
            const auto type = byte();
            if (failed_ || type == 8) return;
            auto key = strings_.empty() ? cstring() : indexed();
            KeyValue child;
            switch (type) {
                case 0:
                    read_children(child, depth + 1);
                    break;
                case 1:
                    child.text = cstring();
                    break;
                case 2:
                case 3:
                case 4:
                case 6:
                    skip(4);
                    break;
                case 5:
                    wide();
                    break;
                case 7:
                case 10:
                    skip(8);
                    break;
                default:
                    failed_ = true;
                    return;
            }
            into.children[lower(key)] = std::move(child);
        }
    }

private:
    std::uint8_t byte() {
        if (position_ >= data_.size()) {
            failed_ = true;
            return 0;
        }
        return data_[position_++];
    }
    void skip(std::size_t count) {
        if (position_ + count > data_.size())
            failed_ = true;
        else
            position_ += count;
    }
    std::string cstring() {
        std::string out;
        while (!failed_) {
            const auto c = byte();
            if (c == 0) break;
            out += static_cast<char>(c);
        }
        return out;
    }
    void wide() {
        while (!failed_) {
            const auto low = byte();
            const auto high = byte();
            if (low == 0 && high == 0) return;
        }
    }
    std::string indexed() {
        std::uint32_t index = 0;
        for (int i = 0; i < 4; ++i) index |= static_cast<std::uint32_t>(byte()) << (8 * i);
        if (failed_ || index >= strings_.size()) {
            failed_ = true;
            return {};
        }
        return strings_[index];
    }

    const std::vector<std::uint8_t>& data_;
    const std::vector<std::string>& strings_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

std::map<std::string, std::vector<std::string>> steam_launch_configuration(
    const fs::path& appinfo, const std::set<std::string>& wanted) {
    std::map<std::string, std::vector<std::string>> launches;
    std::ifstream in(appinfo, std::ios::binary);
    if (!in || wanted.empty()) return launches;
    const auto u32 = [&] {
        std::uint32_t value = 0;
        in.read(reinterpret_cast<char*>(&value), sizeof(value));
        return value;
    };
    const auto magic = u32();
    if ((magic >> 8) != 0x075644u) return launches;
    const bool indexed = (magic & 0xFF) >= 0x29;
    u32();
    std::vector<std::string> strings;
    if (indexed) {
        std::int64_t table = 0;
        in.read(reinterpret_cast<char*>(&table), sizeof(table));
        const auto resume = in.tellg();
        in.seekg(table);
        const auto count = u32();
        if (!in || count > 2'000'000) return launches;
        for (std::uint32_t i = 0; i < count && in; ++i) {
            std::string value;
            for (char c; in.get(c) && c != '\0';) value += c;
            strings.push_back(std::move(value));
        }
        in.clear();
        in.seekg(resume);
    }
    constexpr std::size_t kHeader = 60;
    while (in) {
        const auto appid = u32();
        if (!in || appid == 0) break;
        const auto size = u32();
        if (!in || size < kHeader) break;
        const auto next = in.tellg() + static_cast<std::streamoff>(size);
        if (!wanted.contains(std::to_string(appid))) {
            in.seekg(next);
            continue;
        }
        std::vector<std::uint8_t> data(size);
        in.read(reinterpret_cast<char*>(data.data()), size);
        if (!in) break;
        const std::vector<std::uint8_t> body(data.begin() + kHeader, data.end());
        KeyValue root;
        BinaryReader reader(body, strings);
        reader.read_children(root, 0);
        const auto* info = root.children.contains("appinfo") ? &root.children.at("appinfo") : &root;
        const auto config = info->children.find("config");
        if (config == info->children.end()) continue;
        const auto launch = config->second.children.find("launch");
        if (launch == config->second.children.end()) continue;
        for (const auto& [index, option] : launch->second.children) {
            const auto executable = option.children.find("executable");
            if (executable == option.children.end() || !lower(executable->second.text).ends_with(".exe"))
                continue;
            if (const auto settings = option.children.find("config"); settings != option.children.end()) {
                if (const auto os = settings->second.children.find("oslist");
                    os != settings->second.children.end() &&
                    lower(os->second.text).find("windows") == std::string::npos)
                    continue;
            }
            launches[std::to_string(appid)].push_back(executable->second.text);
        }
    }
    return launches;
}

void provide_steam(ProviderContext& ctx) {
    const auto installs = steam_installs();
    if (installs.empty()) {
        ctx.available = false;
        ctx.error = "Steam is not installed for any signed-in user";
        return;
    }
    std::vector<fs::path> libraries = installs;
    for (const auto& install : installs) {
        const auto tokens =
            quoted_tokens(read_text(install / L"steamapps" / L"libraryfolders.vdf", 1u << 20));
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i] != "path") continue;
            const auto library = path_from_utf8(tokens[i + 1]).lexically_normal();
            if (std::ranges::find(libraries, library) == libraries.end()) libraries.push_back(library);
        }
    }
    std::vector<Claim> pending;
    for (const auto& library : libraries) {
        if (ctx.cancelled()) return;
        std::error_code ec;
        if (!is_local_disk_path(library) || !ctx.volume_ready(library) ||
            !fs::is_directory(library / L"steamapps", ec)) {
            ctx.note += "library " + path_to_utf8(library) + " is not reachable; ";
            continue;
        }
        for (fs::directory_iterator
                 it(library / L"steamapps", fs::directory_options::skip_permission_denied, ec),
             end;
             it != end && !ec; it.increment(ec)) {
            const auto file = path_to_utf8(it->path().filename());
            if (!file.starts_with("appmanifest_") || !file.ends_with(".acf")) continue;
            const auto tokens = quoted_tokens(read_text(it->path(), 256 * 1024));
            const auto install_dir = vdf_value(tokens, "installdir");
            const auto name = vdf_value(tokens, "name");
            const auto root = library / L"steamapps" / L"common" / path_from_utf8(install_dir);
            if (install_dir.empty() || !fs::is_directory(root, ec)) {
                ctx.discard(it->path(), "Steam manifest names an install folder that does not exist");
                continue;
            }
            const auto state = vdf_value(tokens, "StateFlags");
            if (!state.empty() && (std::strtoul(state.c_str(), nullptr, 10) & 4) == 0) {
                ctx.discard(it->path(), "Steam game is not fully installed");
                continue;
            }
            Claim claim;
            claim.source = "steam";
            claim.platform = "steam";
            claim.product_id = vdf_value(tokens, "appid");
            claim.name = name.empty() ? install_dir : name;
            claim.name_rank = 6;
            claim.scope = "machine";
            claim.root = root;
            claim.evidence.push_back({"steam.manifest", path_to_utf8(it->path())});
            pending.push_back(std::move(claim));
        }
    }
    std::set<std::string> wanted;
    for (const auto& claim : pending) wanted.insert(claim.product_id);
    std::map<std::string, std::vector<std::string>> launches;
    for (const auto& install : installs) {
        auto found = steam_launch_configuration(install / L"appcache" / L"appinfo.vdf", wanted);
        launches.merge(found);
    }
    for (auto& claim : pending) {
        if (const auto it = launches.find(claim.product_id); it != launches.end()) {
            int weight = 95;
            for (const auto& relative : it->second) {
                const auto file = claim.root / path_from_utf8(relative);
                if (usable_executable(file)) claim.executables.emplace_back(file, weight);
                weight = 85;
            }
            if (!claim.executables.empty())
                claim.evidence.push_back({"steam.launch", "launch configuration of the Steam app"});
        }
        ctx.claim(std::move(claim));
    }
}

void provide_epic(ProviderContext& ctx) {
    auto data = registry_text(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Epic Games\\EpicGamesLauncher",
                              L"AppDataPath");
    fs::path manifests =
        data.empty() ? environment_path(L"ProgramData") / L"Epic\\EpicGamesLauncher\\Data" : fs::path(data);
    manifests /= L"Manifests";
    std::error_code ec;
    if (!fs::is_directory(manifests, ec)) {
        ctx.available = false;
        ctx.error = "the Epic Games Launcher manifest folder does not exist";
        return;
    }
    for (fs::directory_iterator it(manifests, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (lower(it->path().extension().string()) != ".item") continue;
        const auto json = core::Json::parse(read_text(it->path(), 1u << 20), nullptr, false);
        if (json.is_discarded() || !json.is_object()) continue;
        const auto location = path_from_utf8(json.value("InstallLocation", ""));
        const auto app_name = json.value("AppName", "");
        if (json.value("bIsIncompleteInstall", false) || !fs::is_directory(location, ec)) {
            ctx.discard(it->path(), "Epic manifest describes an incomplete install or a missing folder");
            continue;
        }
        if (!json.value("MainGameAppName", app_name).empty() &&
            json.value("MainGameAppName", app_name) != app_name) {
            ctx.discard(it->path(), "Epic manifest describes downloadable content");
            continue;
        }
        Claim claim;
        claim.source = "epic";
        claim.platform = "epic";
        claim.product_id = app_name;
        claim.name = json.value("DisplayName", app_name);
        claim.version = json.value("AppVersionString", "");
        claim.name_rank = 6;
        claim.scope = "machine";
        claim.root = location;
        if (const auto launch = json.value("LaunchExecutable", ""); !launch.empty())
            claim.executables.emplace_back(location / path_from_utf8(launch), 95);
        claim.evidence.push_back({"epic.manifest", path_to_utf8(it->path())});
        ctx.claim(std::move(claim));
    }
}

void provide_gog(ProviderContext& ctx) {
    const std::wstring parent = L"SOFTWARE\\WOW6432Node\\GOG.com\\Games";
    const auto names = subkeys(HKEY_LOCAL_MACHINE, parent);
    if (names.empty()) {
        ctx.available = false;
        ctx.error = "GOG Galaxy has no registered games";
        return;
    }
    for (const auto& id : names) {
        const auto key = parent + L"\\" + id;
        const auto path = expand(registry_text(HKEY_LOCAL_MACHINE, key, L"path"));
        std::error_code ec;
        if (path.empty() || !fs::is_directory(path, ec)) {
            ctx.discard(path, "GOG registration names a folder that does not exist");
            continue;
        }
        Claim claim;
        claim.source = "gog";
        claim.platform = "gog";
        claim.product_id = to_utf8(id);
        claim.name = to_utf8(registry_text(HKEY_LOCAL_MACHINE, key, L"gameName"));
        claim.version = to_utf8(registry_text(HKEY_LOCAL_MACHINE, key, L"ver"));
        claim.name_rank = 6;
        claim.scope = "machine";
        claim.root = path;
        if (const auto exe = expand(registry_text(HKEY_LOCAL_MACHINE, key, L"exe")); !exe.empty())
            claim.executables.emplace_back(exe, 95);
        claim.evidence.push_back({"gog.registry", to_utf8(key)});
        ctx.claim(std::move(claim));
    }
}

void provide_ubisoft(ProviderContext& ctx) {
    const std::wstring parent = L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher\\Installs";
    const auto ids = subkeys(HKEY_LOCAL_MACHINE, parent);
    if (ids.empty()) {
        ctx.available = false;
        ctx.error = "Ubisoft Connect has no registered games";
        return;
    }
    for (const auto& id : ids) {
        const auto dir = expand(registry_text(HKEY_LOCAL_MACHINE, parent + L"\\" + id, L"InstallDir"));
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec)) {
            ctx.discard(dir, "Ubisoft registration names a folder that does not exist");
            continue;
        }
        Claim claim;
        claim.source = "ubisoft";
        claim.platform = "ubisoft";
        claim.product_id = to_utf8(id);
        claim.name = to_utf8(
            registry_text(HKEY_LOCAL_MACHINE,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Uplay Install " + id,
                          L"DisplayName", KEY_WOW64_32KEY));
        if (claim.name.empty()) {
            const auto normal = fs::path(dir).lexically_normal();
            claim.name =
                path_to_utf8(normal.filename().empty() ? normal.parent_path().filename() : normal.filename());
        }
        claim.name_rank = 5;
        claim.scope = "machine";
        claim.root = dir;
        claim.evidence.push_back({"ubisoft.registry", to_utf8(parent + L"\\" + id)});
        ctx.claim(std::move(claim));
    }
}

void provide_ea(ProviderContext& ctx) {
    bool any = false;
    for (const auto* parent : {L"SOFTWARE\\WOW6432Node\\EA Games", L"SOFTWARE\\WOW6432Node\\Origin Games"}) {
        for (const auto& name : subkeys(HKEY_LOCAL_MACHINE, parent)) {
            const auto dir = expand(
                registry_text(HKEY_LOCAL_MACHINE, std::wstring(parent) + L"\\" + name, L"Install Dir"));
            std::error_code ec;
            if (dir.empty() || !fs::is_directory(dir, ec)) {
                ctx.discard(dir, "EA registration names a folder that does not exist");
                continue;
            }
            any = true;
            Claim claim;
            claim.source = "ea";
            claim.platform = "ea";
            claim.product_id = to_utf8(name);
            claim.name = to_utf8(name);
            claim.name_rank = 4;
            claim.scope = "machine";
            claim.root = dir;
            claim.evidence.push_back({"ea.registry", to_utf8(std::wstring(parent) + L"\\" + name)});
            ctx.claim(std::move(claim));
        }
    }
    if (!any) {
        ctx.available = false;
        ctx.error = "the EA app has no registered games";
    }
}

void provide_battlenet(ProviderContext& ctx) {
    ctx.available = false;
    ctx.error =
        "Battle.net keeps its install list in a private database; its games are found through the Windows "
        "uninstall list";
}

void provide_package_managers(ProviderContext& ctx) {
    std::vector<std::pair<fs::path, const char*>> places;
    for (const auto& profile : user_profiles()) {
        places.emplace_back(profile.directory / L"scoop\\apps", "scoop");
        places.emplace_back(profile.directory / L"AppData\\Local\\Microsoft\\WinGet\\Packages", "winget");
    }
    places.emplace_back(environment_path(L"ProgramData") / L"chocolatey\\lib", "chocolatey");
    places.emplace_back(environment_path(L"ProgramData") / L"scoop\\apps", "scoop");
    bool any = false;
    for (const auto& [place, manager] : places) {
        std::error_code ec;
        if (place.empty() || !fs::is_directory(place, ec)) continue;
        any = true;
        for (fs::directory_iterator it(place, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (ctx.cancelled()) return;
            std::error_code type_error;
            if (!it->is_directory(type_error)) continue;
            auto root = it->path();
            Claim claim;
            claim.source = "package_managers";
            claim.platform = manager;
            claim.kind = AppKind::Portable;
            claim.scope = "user";
            claim.name_rank = 2;
            claim.name = path_to_utf8(it->path().filename());
            claim.product_id = claim.name;
            if (std::string_view(manager) == "scoop" && fs::is_directory(root / L"current", type_error)) {
                root /= L"current";
                const auto manifest =
                    core::Json::parse(read_text(root / L"manifest.json", 1u << 20), nullptr, false);
                if (manifest.is_object() && manifest.contains("bin")) {
                    const auto& bin = manifest["bin"];
                    const auto add = [&](const core::Json& item) {
                        if (item.is_string() && lower(item.get<std::string>()).ends_with(".exe"))
                            claim.executables.emplace_back(root / path_from_utf8(item.get<std::string>()),
                                                           70);
                    };
                    if (bin.is_array()) {
                        for (const auto& item : bin)
                            add(item.is_array() && !item.empty() ? item.front() : item);
                    } else {
                        add(bin);
                    }
                }
            }
            claim.root = root;
            claim.evidence.push_back({std::string(manager) + ".folder", path_to_utf8(root)});
            ctx.claim(std::move(claim));
        }
    }
    if (!any) {
        ctx.available = false;
        ctx.error = "no winget, Scoop or Chocolatey folders were found";
    }
}

void provide_path(ProviderContext& ctx) {
    std::vector<fs::path> directories;
    const auto append = [&](const std::wstring& list) {
        std::wstringstream stream(list);
        std::wstring part;
        while (std::getline(stream, part, L';')) {
            if (part.empty()) continue;
            auto directory = expand(part);
            const auto lowered = lower(path_to_utf8(directory)) + "\\";
            if (under_windows(lowered) || lowered.find("\\windowsapps\\") != std::string::npos) continue;
            if (std::ranges::find(directories, directory) == directories.end())
                directories.push_back(std::move(directory));
        }
    };
    append(registry_text(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"Path"));
    for (const auto& hive : loaded_user_hives())
        append(registry_text(HKEY_USERS, hive + L"\\Environment", L"Path"));
    for (const auto& directory : directories) {
        if (ctx.cancelled()) return;
        std::error_code ec;
        if (!is_local_disk_path(directory) || !fs::is_directory(directory, ec)) continue;
        std::size_t seen = 0;
        for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec && seen < 400; it.increment(ec), ++seen) {
            if (lower(it->path().extension().string()) != ".exe") continue;
            Claim claim;
            claim.source = "path";
            claim.name = path_to_utf8(it->path().stem());
            claim.name_rank = 1;
            claim.user_facing = false;
            claim.root = directory;
            claim.executables.emplace_back(it->path(), 20);
            claim.evidence.push_back({"path", path_to_utf8(directory)});
            ctx.claim(std::move(claim));
        }
    }
}

void provide_portable(ProviderContext& ctx) {
    for (const auto& root : ctx.request().portable_roots) {
        if (ctx.cancelled()) return;
        if (!ctx.volume_ready(root)) {
            ctx.note += "folder " + path_to_utf8(root) + " is on a volume that is not ready; ";
            continue;
        }
        for (auto& claim : group_installations(root, "portable_folders", ctx.request().cancel))
            ctx.claim(std::move(claim));
    }
}

void provide_manual(ProviderContext& ctx) {
    for (const auto& file : ctx.request().manual) {
        if (!usable_executable(file)) {
            ctx.discard(file, "hand-added program is missing or not on a local disk");
            continue;
        }
        Claim claim;
        claim.source = "manual";
        claim.kind = AppKind::Manual;
        claim.name = path_to_utf8(file.stem());
        claim.name_rank = 7;
        claim.root = file.parent_path();
        claim.executables.emplace_back(file, 100);
        claim.evidence.push_back({"manual", "added by the owner"});
        ctx.claim(std::move(claim));
    }
}

void provide_survey(ProviderContext& ctx) {
    constexpr std::size_t kContainerChildren = 3;
    for (const auto& volume : ctx.volumes()) {
        if (!volume.included || !volume.ready || ctx.cancelled()) continue;
        const fs::path root = fs::path(to_wide(volume.root));
        std::error_code ec;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (ctx.cancelled()) return;
            std::error_code type_error;
            if (!it->is_directory(type_error) || skip_survey_directory(it->path().filename().wstring()) ||
                ctx.covered(canonical_key(it->path())))
                continue;
            WalkBudget budget;
            budget.max_entries = 60000;
            budget.max_time = std::chrono::seconds(20);
            budget.cancel = ctx.request().cancel;
            const auto walked = find_executables(it->path(), budget, skip_survey_directory);
            if (walked.executables.empty()) continue;

            std::map<std::string, std::vector<FileEntry>> children;
            std::size_t direct = 0;
            for (const auto& exe : walked.executables) {
                const auto relative = fs::relative(exe.path, it->path());
                if (std::distance(relative.begin(), relative.end()) == 1) {
                    ++direct;
                    continue;
                }
                children[path_to_utf8(*relative.begin())].push_back(exe);
            }
            const bool container = direct == 0 && children.size() >= kContainerChildren;
            const auto emit = [&](const fs::path& folder, std::vector<FileEntry> executables) {
                if (ctx.covered(canonical_key(folder))) return;
                Claim claim;
                claim.source = "volume_survey";
                claim.kind = AppKind::Portable;
                claim.name = path_to_utf8(folder.filename());
                claim.name_rank = 0;
                claim.scope = "machine";
                claim.root = folder;
                if (executables.size() > 400) executables.resize(400);
                claim.known_executables = std::move(executables);
                claim.evidence.push_back({"volume_survey", "found while surveying " + volume.root});
                ctx.claim(std::move(claim));
            };
            if (container) {
                for (auto& [name, executables] : children)
                    emit(it->path() / path_from_utf8(name), std::move(executables));
            } else {
                emit(it->path(), walked.executables);
            }
        }
    }
}

}  // namespace

void ProviderContext::claim(Claim claim) {
    claim.source = source;
    if (!claim.root.empty()) covered_.insert(canonical_key(claim.root));
    for (const auto& [file, weight] : claim.executables) covered_.insert(canonical_key(file));
    claims.push_back(std::move(claim));
}

void ProviderContext::seed(const std::vector<Claim>& earlier) {
    for (const auto& claim : earlier) {
        if (!claim.root.empty()) covered_.insert(canonical_key(claim.root));
        for (const auto& [file, weight] : claim.executables) covered_.insert(canonical_key(file));
    }
}

void ProviderContext::discard(const fs::path& path, std::string reason) {
    ++discard_count;
    if (discarded.size() < kMaxDiscardLog)
        discarded.push_back({path_to_utf8(path), std::move(reason), source});
}

bool ProviderContext::covered(const std::string& key) const {
    if (covered_.contains(key)) return true;
    return std::ranges::any_of(covered_, [&](const std::string& root) { return key_inside(root, key); });
}

bool ProviderContext::volume_ready(const fs::path& path) const {
    const auto volume = volume_of(path);
    if (volume.empty()) return false;
    return std::ranges::any_of(volumes_, [&](const VolumeInfo& info) {
        return info.root.starts_with(volume) && info.ready && info.included;
    });
}

std::vector<Claim> group_installations(const fs::path& folder, const char* source,
                                       const std::atomic<bool>* cancel) {
    std::vector<Claim> claims;
    std::error_code ec;
    if (!is_local_disk_path(folder) || !fs::is_directory(folder, ec)) return claims;
    const auto make = [&](const fs::path& root, std::vector<FileEntry> executables) {
        Claim claim;
        claim.source = source;
        claim.kind = AppKind::Portable;
        claim.name = path_to_utf8(root.filename());
        claim.name_rank = 0;
        claim.root = root;
        claim.known_executables = std::move(executables);
        claim.evidence.push_back({"folder", path_to_utf8(folder)});
        return claim;
    };
    std::vector<FileEntry> top_level;
    std::vector<fs::path> subfolders;
    for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            const auto name = lower(path_to_utf8(it->path().filename()));
            DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (!name.starts_with('.') && name != "node_modules" && attributes != INVALID_FILE_ATTRIBUTES &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                subfolders.push_back(it->path());
        } else if (lower(it->path().extension().string()) == ".exe") {
            top_level.push_back({it->path(), it->file_size(type_error), 0});
        }
    }
    const bool app_folder = std::ranges::any_of(top_level, [](const FileEntry& entry) {
        return classify_executable(entry.path, {}) == AppRole::Application &&
               pe_subsystem(entry.path) == Subsystem::Gui;
    });
    if (app_folder) {
        WalkBudget budget;
        budget.cancel = cancel;
        claims.push_back(make(folder, find_executables(folder, budget).executables));
        return claims;
    }
    for (const auto& subfolder : subfolders) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) break;
        WalkBudget budget;
        budget.max_entries = 4000;
        budget.cancel = cancel;
        auto walked = find_executables(subfolder, budget);
        if (!walked.executables.empty()) claims.push_back(make(subfolder, std::move(walked.executables)));
    }
    for (const auto& entry : top_level) {
        Claim claim;
        claim.source = source;
        claim.kind = AppKind::Portable;
        claim.name = path_to_utf8(entry.path.stem());
        claim.name_rank = 0;
        claim.root = folder;
        claim.executables.emplace_back(entry.path, 30);
        claim.evidence.push_back({"folder", path_to_utf8(folder)});
        claims.push_back(std::move(claim));
    }
    return claims;
}

const std::vector<Provider>& providers() {
    static const std::vector<Provider> list = {
        {"windows_registry", "Uninstall entries of the machine and every loaded user", false,
         provide_registry},
        {"app_paths", "App Paths registrations", false, provide_app_paths},
        {"shortcuts", "Start menu and desktop shortcuts of every user", false, provide_shortcuts},
        {"packages", "Microsoft Store and packaged apps from their manifests", false, provide_packages},
        {"xbox_games", "Xbox games installed in XboxGames folders on any volume", false, provide_xbox},
        {"steam", "Steam libraries on any volume, from libraryfolders and app manifests", false,
         provide_steam},
        {"epic", "Epic Games Launcher manifests", false, provide_epic},
        {"gog", "GOG Galaxy registrations", false, provide_gog},
        {"ubisoft", "Ubisoft Connect registrations", false, provide_ubisoft},
        {"ea", "EA app and Origin registrations", false, provide_ea},
        {"battlenet", "Battle.net (limited)", false, provide_battlenet},
        {"package_managers", "winget, Scoop and Chocolatey folders", false, provide_package_managers},
        {"path", "Programs on the PATH", false, provide_path},
        {"portable_folders", "Folders registered by the owner", false, provide_portable},
        {"manual", "Programs added by hand", false, provide_manual},
        {"volume_survey", "Deep scan of fixed volumes for programs without registration", true,
         provide_survey},
    };
    return list;
}

}  // namespace houseguest::system::catalog
