#include "houseguest/system/app_catalog.hpp"

// clang-format off
#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

namespace houseguest::system {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxApps = 6000;
constexpr std::size_t kMaxFolderEntries = 4000;

std::string lower(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string alnum(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
    }
    return out;
}

bool cancelled(const std::atomic<bool>* flag) {
    return flag != nullptr && flag->load(std::memory_order_relaxed);
}

fs::path expand(std::wstring text) {
    text.erase(std::remove(text.begin(), text.end(), L'"'), text.end());
    wchar_t expanded[MAX_PATH * 2] = {};
    const auto length = ExpandEnvironmentStringsW(text.c_str(), expanded, MAX_PATH * 2);
    if (length == 0 || length > MAX_PATH * 2) return fs::path(text);
    return expanded;
}

fs::path environment_path(const wchar_t* name) {
    wchar_t buffer[MAX_PATH * 2] = {};
    const auto length = GetEnvironmentVariableW(name, buffer, MAX_PATH * 2);
    return length == 0 || length >= MAX_PATH * 2 ? fs::path() : fs::path(buffer);
}

std::wstring registry_text(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view = 0) {
    wchar_t buffer[MAX_PATH * 4] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ |
                        (view == KEY_WOW64_32KEY   ? RRF_SUBKEY_WOW6432KEY
                         : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY
                                                   : 0);
    if (RegGetValueW(root, key.c_str(), value, flags, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return buffer;
}

DWORD registry_number(HKEY root, const std::wstring& key, const wchar_t* value, REGSAM view = 0) {
    DWORD number = 0, size = sizeof(number);
    const DWORD flags = RRF_RT_REG_DWORD | (view == KEY_WOW64_32KEY   ? RRF_SUBKEY_WOW6432KEY
                                            : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY
                                                                      : 0);
    RegGetValueW(root, key.c_str(), value, flags, nullptr, &number, &size);
    return number;
}

std::vector<std::wstring> subkeys(HKEY root, const std::wstring& key, REGSAM view = 0) {
    std::vector<std::wstring> names;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &handle) != ERROR_SUCCESS) return names;
    wchar_t name[256];
    for (DWORD index = 0; names.size() < 8000; ++index) {
        DWORD size = 256;
        if (RegEnumKeyExW(handle, index, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        names.emplace_back(name, size);
    }
    RegCloseKey(handle);
    return names;
}

fs::path resolve_shortcut(const fs::path& link) {
    IShellLinkW* shell_link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                reinterpret_cast<void**>(&shell_link)))) {
        return {};
    }
    fs::path target;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(shell_link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
        if (SUCCEEDED(file->Load(link.c_str(), STGM_READ))) {
            wchar_t buffer[MAX_PATH * 2] = {};
            if (SUCCEEDED(shell_link->GetPath(buffer, MAX_PATH * 2, nullptr, SLGP_RAWPATH)))
                target = expand(buffer);
        }
        file->Release();
    }
    shell_link->Release();
    return target;
}

std::string version_string(const fs::path& file, const wchar_t* field) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0) return {};
    std::vector<std::uint8_t> block(size);
    if (!GetFileVersionInfoW(file.c_str(), 0, size, block.data())) return {};
    struct Translation {
        WORD language;
        WORD code_page;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations),
                        &length) ||
        length < sizeof(Translation)) {
        return {};
    }
    wchar_t query[96];
    swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].code_page,
               field);
    wchar_t* value = nullptr;
    if (!VerQueryValueW(block.data(), query, reinterpret_cast<void**>(&value), &length) || value == nullptr)
        return {};
    return to_utf8(value);
}

std::int64_t to_epoch(const fs::file_time_type& stamp) {
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(stamp);
    return std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count();
}

bool inside(const std::string& lowered_path, const char* fragment) {
    return lowered_path.find(fragment) != std::string::npos;
}

bool under_windows(const std::string& lowered_path) {
    static const std::string windows = [] {
        wchar_t buffer[MAX_PATH] = {};
        GetWindowsDirectoryW(buffer, MAX_PATH);
        return lower(to_utf8(buffer)) + "\\";
    }();
    return lowered_path.starts_with(windows);
}

std::string long_path_key(const fs::path& file) {
    wchar_t buffer[MAX_PATH * 2] = {};
    const auto length = GetLongPathNameW(file.c_str(), buffer, MAX_PATH * 2);
    auto text = to_utf8(length != 0 && length < MAX_PATH * 2 ? std::wstring(buffer) : file.wstring());
    std::ranges::replace(text, '/', '\\');
    return lower(std::move(text));
}

bool usable_executable(const fs::path& file) {
    std::error_code ec;
    return lower(file.extension().string()) == ".exe" && is_local_disk_path(file) &&
           fs::is_regular_file(file, ec);
}

struct Rank {
    static constexpr int manual = 6;
    static constexpr int store = 5;
    static constexpr int registry = 4;
    static constexpr int shortcut = 3;
    static constexpr int app_paths = 2;
    static constexpr int discovered = 1;
};

class Collector {
public:
    void add(InstalledApp app, int name_rank, bool user_facing) {
        if (app.path.empty() || apps_.size() >= kMaxApps) return;
        const auto file = path_from_utf8(app.path);
        const auto key = long_path_key(file);
        app.role = classify_executable(file, app.name);
        if (app.role == AppRole::Application && !user_facing)
            app.role = AppRole::Tool;
        else if (app.role == AppRole::Tool && user_facing)
            app.role = AppRole::Application;
        if (const auto found = index_.find(key); found != index_.end()) {
            merge(apps_[found->second], ranks_[found->second], std::move(app), name_rank);
            return;
        }
        index_.emplace(key, apps_.size());
        ranks_.push_back(name_rank);
        apps_.push_back(std::move(app));
    }

    bool covers(const fs::path& directory) const {
        auto prefix = long_path_key(directory);
        if (!prefix.ends_with('\\')) prefix += '\\';
        return std::ranges::any_of(apps_, [&](const InstalledApp& app) {
            return lower(app.path).starts_with(prefix) ||
                   lower(app.install_dir).starts_with(prefix.substr(0, prefix.size() - 1));
        });
    }

    const std::vector<InstalledApp>& apps() const { return apps_; }
    std::vector<InstalledApp> take() { return std::move(apps_); }

private:
    static void merge(InstalledApp& into, int& rank, InstalledApp&& other, int name_rank) {
        for (auto& source : other.sources) {
            if (std::ranges::find(into.sources, source) == into.sources.end()) into.sources.push_back(source);
        }
        if (name_rank > rank && !other.name.empty()) {
            into.name = other.name;
            rank = name_rank;
        }
        const auto fill = [](std::string& target, std::string& value) {
            if (target.empty()) target = std::move(value);
        };
        fill(into.company, other.company);
        fill(into.version, other.version);
        fill(into.description, other.description);
        fill(into.package_family, other.package_family);
        fill(into.install_dir, other.install_dir);
        if (other.kind == AppKind::Store)
            into.kind = AppKind::Store;
        else if (into.kind != AppKind::Store && other.kind == AppKind::Installed &&
                 into.kind == AppKind::Portable)
            into.kind = AppKind::Installed;
        if (other.kind == AppKind::Manual) into.kind = AppKind::Manual;
        if (into.scope.empty()) into.scope = other.scope;
        if (other.role == AppRole::Application) into.role = AppRole::Application;
        into.source = into.sources.front();
    }

    std::vector<InstalledApp> apps_;
    std::vector<int> ranks_;
    std::unordered_map<std::string, std::size_t> index_;
};

InstalledApp make_app(std::string name, const fs::path& target, const char* source, AppKind kind,
                      std::string scope) {
    InstalledApp app;
    app.name = std::move(name);
    app.path = path_to_utf8(target);
    app.company = version_string(target, L"CompanyName");
    app.version = version_string(target, L"FileVersion");
    app.description = version_string(target, L"FileDescription");
    app.install_dir = path_to_utf8(target.parent_path());
    app.source = source;
    app.sources = {source};
    app.kind = kind;
    app.scope = std::move(scope);
    std::error_code ec;
    app.size = static_cast<std::int64_t>(fs::file_size(target, ec));
    if (ec) app.size = 0;
    const auto stamp = fs::last_write_time(target, ec);
    app.modified = ec ? 0 : to_epoch(stamp);
    if (app.name.empty()) app.name = !app.description.empty() ? app.description : path_to_utf8(target.stem());
    return app;
}

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
        if (skipped.contains(lower(name))) continue;
        profiles.push_back({entry.path(), name});
    }
    return profiles;
}

std::vector<std::pair<std::wstring, std::string>> loaded_user_hives() {
    std::vector<std::pair<std::wstring, std::string>> hives;
    for (const auto& name : subkeys(HKEY_USERS, L"")) {
        if (name.starts_with(L"S-1-5-21-") && !name.ends_with(L"_Classes"))
            hives.emplace_back(name, to_utf8(name));
    }
    return hives;
}

bool skip_shortcut_name(const std::string& lowered) {
    static const char* const noise[] = {"uninstall",     "desinstal", "remove ", "readme", "documentation",
                                        "release notes", "license",   "website", "help"};
    return std::ranges::any_of(noise, [&](const char* word) { return inside(lowered, word); });
}

struct Candidate {
    fs::path file;
    int score = 0;
};

std::string acronym(const std::string& text) {
    std::string out;
    bool start = true;
    for (const unsigned char c : text) {
        if (!std::isalnum(c)) {
            start = true;
            continue;
        }
        if (start) out += static_cast<char>(std::tolower(c));
        start = false;
    }
    return out.size() >= 2 ? out : std::string{};
}

std::vector<Candidate> main_executables(const fs::path& directory, const std::string& hint, unsigned depth,
                                        const std::atomic<bool>* cancel) {
    std::vector<Candidate> found;
    const auto wanted = alnum(hint);
    const auto initials = acronym(hint);
    const auto folder_name = alnum(path_to_utf8(directory.filename()));
    std::size_t visited = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec),
         end;
         it != end && !ec && visited < kMaxFolderEntries; it.increment(ec)) {
        if (cancelled(cancel)) break;
        ++visited;
        std::error_code type_error;
        if (it->is_directory(type_error)) {
            const auto subfolder = lower(path_to_utf8(it->path().filename()));
            if (it.depth() >= static_cast<int>(depth) || subfolder == "node_modules" ||
                subfolder == "resources" || subfolder == "locales" || subfolder == "_internal" ||
                subfolder.starts_with('.')) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (lower(it->path().extension().string()) != ".exe") continue;
        const auto stem = path_to_utf8(it->path().stem());
        if (classify_executable(it->path(), stem) != AppRole::Application) continue;
        int score = 10 - 2 * it.depth();
        const auto key = alnum(stem);
        if (!initials.empty() && key == initials) score += 10;
        if (!wanted.empty() && (key == wanted || wanted.starts_with(key) || key.starts_with(wanted)))
            score += 8;
        else if (!wanted.empty() && !key.empty() &&
                 (wanted.find(key) != std::string::npos || key.find(wanted) != std::string::npos))
            score += 4;
        if (!folder_name.empty() &&
            (key == folder_name || folder_name.starts_with(key) || key.starts_with(folder_name)))
            score += 6;
        found.push_back({it->path(), score});
    }
    std::ranges::stable_sort(found, [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
    return found;
}

using Report = std::vector<SourceReport>;

template <typename Work>
void run_source(Report& reports, const char* name, Collector& collector, const CatalogRequest& request,
                Work&& work) {
    SourceReport report;
    report.name = name;
    const auto before = collector.apps().size();
    const auto started = std::chrono::steady_clock::now();
    try {
        work(report);
    } catch (const std::exception& error) {
        report.available = false;
        report.error = error.what();
    }
    report.found = collector.apps().size() - before;
    report.milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    reports.push_back(std::move(report));
    if (request.progress) request.progress(collector.apps());
}

void scan_uninstall_key(Collector& collector, HKEY root, const std::wstring& parent, REGSAM view,
                        const char* scope, const std::atomic<bool>* cancel) {
    for (const auto& name : subkeys(root, parent, view)) {
        if (cancelled(cancel)) return;
        const auto key = parent + L"\\" + name;
        if (registry_number(root, key, L"SystemComponent", view) != 0) continue;
        if (!registry_text(root, key, L"ParentKeyName", view).empty()) continue;
        const auto display = registry_text(root, key, L"DisplayName", view);
        if (display.empty()) continue;

        fs::path target;
        auto icon = registry_text(root, key, L"DisplayIcon", view);
        if (const auto comma = icon.rfind(L','); comma != std::wstring::npos) icon.resize(comma);
        if (!icon.empty()) {
            auto candidate = expand(icon);
            const auto lowered = lower(path_to_utf8(candidate));
            if (usable_executable(candidate) && !inside(lowered, "\\windows\\installer\\") &&
                classify_executable(candidate, path_to_utf8(candidate.stem())) != AppRole::Installer) {
                target = std::move(candidate);
            }
        }
        const auto location = expand(registry_text(root, key, L"InstallLocation", view));
        if (target.empty() && !location.empty()) {
            std::error_code ec;
            if (fs::is_directory(location, ec) && is_local_disk_path(location)) {
                const auto found = main_executables(location, to_utf8(display), 4, cancel);
                if (!found.empty() && found.front().score >= 8) target = found.front().file;
            }
        }
        if (target.empty()) continue;
        auto app = make_app(to_utf8(display), target, "registry", AppKind::Installed, scope);
        if (const auto publisher = to_utf8(registry_text(root, key, L"Publisher", view)); app.company.empty())
            app.company = publisher;
        if (const auto version = to_utf8(registry_text(root, key, L"DisplayVersion", view)); !version.empty())
            app.version = version;
        if (!location.empty()) app.install_dir = path_to_utf8(location);
        collector.add(std::move(app), Rank::registry, true);
    }
}

void scan_registry(Collector& collector, const std::atomic<bool>* cancel) {
    const std::wstring uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    scan_uninstall_key(collector, HKEY_LOCAL_MACHINE, uninstall, KEY_WOW64_64KEY, "machine", cancel);
    scan_uninstall_key(collector, HKEY_LOCAL_MACHINE, uninstall, KEY_WOW64_32KEY, "machine", cancel);
    for (const auto& [hive, sid] : loaded_user_hives()) {
        scan_uninstall_key(collector, HKEY_USERS, hive + L"\\" + uninstall, 0, "user", cancel);
        scan_uninstall_key(collector, HKEY_USERS,
                           hive + L"\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                           0, "user", cancel);
    }
}

void scan_app_paths_key(Collector& collector, HKEY root, const std::wstring& parent, REGSAM view,
                        const char* scope) {
    for (const auto& name : subkeys(root, parent, view)) {
        auto value = registry_text(root, parent + L"\\" + name, nullptr, view);
        if (value.empty()) continue;
        const auto target = expand(value);
        if (!usable_executable(target)) continue;
        collector.add(make_app(path_to_utf8(target.stem()), target, "app_paths", AppKind::Installed, scope),
                      Rank::app_paths, true);
    }
}

void scan_app_paths(Collector& collector) {
    const std::wstring parent = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths";
    scan_app_paths_key(collector, HKEY_LOCAL_MACHINE, parent, KEY_WOW64_64KEY, "machine");
    scan_app_paths_key(collector, HKEY_LOCAL_MACHINE, parent, KEY_WOW64_32KEY, "machine");
    for (const auto& [hive, sid] : loaded_user_hives())
        scan_app_paths_key(collector, HKEY_USERS, hive + L"\\" + parent, 0, "user");
}

void scan_shortcuts(Collector& collector, const fs::path& root, const char* scope,
                    const std::atomic<bool>* cancel) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (cancelled(cancel)) return;
        std::error_code type_error;
        if (!it->is_regular_file(type_error) || lower(it->path().extension().string()) != ".lnk") continue;
        const auto stem = path_to_utf8(it->path().stem());
        if (skip_shortcut_name(lower(stem))) continue;
        const auto target = resolve_shortcut(it->path());
        if (target.empty() || !usable_executable(target)) continue;
        collector.add(make_app(stem, target, "start_menu", AppKind::Installed, scope), Rank::shortcut, true);
    }
}

void scan_start_menu(Collector& collector, const std::atomic<bool>* cancel) {
    const auto common = environment_path(L"ProgramData");
    if (!common.empty())
        scan_shortcuts(collector, common / L"Microsoft\\Windows\\Start Menu\\Programs", "machine", cancel);
    for (const auto& profile : user_profiles())
        scan_shortcuts(collector,
                       profile.directory / L"AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs",
                       "user", cancel);
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

std::string resolve_resource(const std::wstring& full_name, const std::string& package, std::string display) {
    if (!display.starts_with("ms-resource:")) return display;
    const auto resource = L"@{" + full_name + L"? ms-resource://" + to_wide(package) + L"/resources/" +
                          to_wide(display.substr(12)) + L"}";
    wchar_t resolved[256] = {};
    if (SUCCEEDED(SHLoadIndirectString(resource.c_str(), resolved, 256, nullptr)) && resolved[0] != 0)
        return to_utf8(resolved);
    return {};
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

std::vector<PackageFolder> package_folders(const std::atomic<bool>* cancel) {
    const auto root = environment_path(L"ProgramFiles") / L"WindowsApps";
    std::error_code ec;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) throw std::runtime_error("the WindowsApps folder cannot be listed: " + ec.message());
    std::map<std::string, PackageFolder> newest;
    for (const fs::directory_iterator end; it != end && !ec; it.increment(ec)) {
        if (cancelled(cancel)) break;
        std::error_code type_error;
        if (!it->is_directory(type_error)) continue;
        const auto folder = path_to_utf8(it->path().filename());
        std::vector<std::string> parts;
        std::stringstream stream(folder);
        std::string piece;
        while (std::getline(stream, piece, '_')) parts.push_back(piece);
        if (stream.eof() && !folder.empty() && folder.back() == '_') parts.emplace_back();
        if (parts.size() < 5 || !parts[3].empty()) continue;
        PackageFolder package{it->path().filename().wstring(), parts[0], parts[4], parse_version(parts[1]),
                              it->path()};
        const auto family = package.name + "_" + package.publisher;
        const auto known = newest.find(family);
        if (known == newest.end() || known->second.version < package.version)
            newest[family] = std::move(package);
    }
    std::vector<PackageFolder> list;
    for (auto& [family, package] : newest) list.push_back(std::move(package));
    return list;
}

void scan_packages(Collector& collector, const std::atomic<bool>* cancel) {
    for (const auto& package : package_folders(cancel)) {
        if (cancelled(cancel)) return;
        const auto& full_name = package.full_name;
        const auto& location = package.location;
        if (inside(lower(location.string()), "\\systemapps\\")) continue;
        std::ifstream manifest_file(location / L"AppxManifest.xml", std::ios::binary);
        if (!manifest_file) continue;
        std::ostringstream buffer;
        buffer << manifest_file.rdbuf();
        const auto manifest = buffer.str();
        if (manifest.size() > (1u << 20) || xml_element(manifest, "Framework") == "true") continue;

        const auto identity_at = manifest.find("<Identity ");
        const auto identity =
            identity_at == std::string::npos ? std::string{} : tag_at(manifest, identity_at);
        const auto package_name = xml_attribute(identity, "Name");
        const auto package_display =
            resolve_resource(full_name, package_name, xml_element(manifest, "DisplayName"));
        auto publisher = xml_element(manifest, "PublisherDisplayName");
        if (publisher.starts_with("ms-resource:")) publisher.clear();
        const auto package_family = package.name + "_" + package.publisher;

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
            if (xml_attribute(visual, "AppListEntry") == "none") continue;
            const auto target = location / path_from_utf8(executable);
            if (!usable_executable(target)) continue;
            auto display = resolve_resource(full_name, package_name, xml_attribute(visual, "DisplayName"));
            if (display.empty()) display = package_display;
            if (display.empty()) display = humanize(package_name);
            auto app = make_app(display, target, "store", AppKind::Store, "machine");
            app.company = publisher.empty() ? app.company : publisher;
            app.package_family = package_family;
            app.install_dir = path_to_utf8(location);
            collector.add(std::move(app), Rank::store, true);
        }
    }
}

std::vector<fs::path> path_directories() {
    std::vector<fs::path> directories;
    const auto append = [&](const std::wstring& list) {
        std::wstringstream stream(list);
        std::wstring part;
        while (std::getline(stream, part, L';')) {
            if (part.empty()) continue;
            auto directory = expand(part);
            const auto lowered = lower(path_to_utf8(directory)) + "\\";
            if (under_windows(lowered) || inside(lowered, "\\windowsapps\\")) continue;
            if (std::ranges::find(directories, directory) == directories.end())
                directories.push_back(std::move(directory));
        }
    };
    append(registry_text(HKEY_LOCAL_MACHINE,
                         L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment", L"Path"));
    for (const auto& [hive, sid] : loaded_user_hives())
        append(registry_text(HKEY_USERS, hive + L"\\Environment", L"Path"));
    return directories;
}

void scan_path(Collector& collector, const std::atomic<bool>* cancel) {
    for (const auto& directory : path_directories()) {
        if (cancelled(cancel)) return;
        std::error_code ec;
        if (!is_local_disk_path(directory) || !fs::is_directory(directory, ec)) continue;
        std::size_t seen = 0;
        for (fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec && seen < 400; it.increment(ec), ++seen) {
            if (lower(it->path().extension().string()) != ".exe") continue;
            auto app = make_app(path_to_utf8(it->path().stem()), it->path(), "path", AppKind::Installed, "");
            collector.add(std::move(app), Rank::discovered, false);
        }
    }
}

void scan_conventional(Collector& collector, const std::atomic<bool>* cancel) {
    static const std::set<std::string> skipped = {"common files",
                                                  "windowsapps",
                                                  "modifiablewindowsapps",
                                                  "reference assemblies",
                                                  "msbuild",
                                                  "windowspowershell",
                                                  "internet explorer",
                                                  "windows nt",
                                                  "windows mail",
                                                  "windows photo viewer",
                                                  "windows sidebar",
                                                  "uninstall information",
                                                  "microsoft update health tools",
                                                  "dotnet",
                                                  "packages",
                                                  "temp",
                                                  "microsoft"};
    std::vector<std::pair<fs::path, std::string>> roots = {
        {environment_path(L"ProgramFiles"), "machine"},
        {environment_path(L"ProgramFiles(x86)"), "machine"},
        {environment_path(L"ProgramW6432"), "machine"}};
    for (const auto& profile : user_profiles())
        roots.emplace_back(profile.directory / L"AppData\\Local\\Programs", "user");
    std::set<std::string> done;
    for (const auto& [root, scope] : roots) {
        std::error_code ec;
        if (root.empty() || !done.insert(long_path_key(root)).second || !fs::is_directory(root, ec)) continue;
        for (fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            if (cancelled(cancel)) return;
            std::error_code type_error;
            if (!it->is_directory(type_error) || skipped.contains(lower(path_to_utf8(it->path().filename()))))
                continue;
            if (collector.covers(it->path())) continue;
            const auto found = main_executables(it->path(), path_to_utf8(it->path().filename()), 2, cancel);
            for (std::size_t i = 0; i < found.size() && i < 2; ++i) {
                if (i > 0 && found[i].score < 10) break;
                auto app = make_app({}, found[i].file, "folder", AppKind::Installed, scope);
                if (app.name.empty() || lower(app.name) == lower(path_to_utf8(found[i].file.stem())))
                    app.name =
                        !app.description.empty() ? app.description : path_to_utf8(it->path().filename());
                app.install_dir = path_to_utf8(it->path());
                collector.add(std::move(app), Rank::discovered, true);
            }
        }
    }
}

void scan_package_managers(Collector& collector, const std::atomic<bool>* cancel) {
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
            if (cancelled(cancel)) return;
            std::error_code type_error;
            if (!it->is_directory(type_error)) continue;
            auto root = it->path();
            if (std::string_view(manager) == "scoop" && fs::is_directory(root / L"current", type_error))
                root /= L"current";
            if (collector.covers(root)) continue;
            const auto found = main_executables(root, path_to_utf8(it->path().filename()), 2, cancel);
            if (found.empty() || found.front().score < 8) continue;
            auto app = make_app({}, found.front().file, manager, AppKind::Portable, "user");
            app.install_dir = path_to_utf8(root);
            collector.add(std::move(app), Rank::discovered, true);
        }
    }
    if (!any) throw std::runtime_error("no package manager folders found");
}

void scan_roots(Collector& collector, const std::vector<fs::path>& roots, const char* source, AppKind kind,
                const std::atomic<bool>* cancel) {
    for (const auto& root : roots) {
        if (cancelled(cancel)) return;
        for (auto& app : scan_folder(root, 3, cancel)) {
            app.kind = kind;
            app.source = source;
            app.sources = {source};
            collector.add(std::move(app), Rank::discovered, true);
        }
    }
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

AppRole classify_executable(const fs::path& file, const std::string& name) {
    const auto stem = lower(path_to_utf8(file.stem()));
    const auto label = lower(name);
    const auto path = lower(path_to_utf8(file));
    const auto starts = [&](const char* prefix) { return stem.starts_with(prefix); };
    const auto has = [&](const char* part) { return inside(stem, part); };

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
        inside(path, "\\driverstore\\") || inside(path, "\\system32\\drivers\\") || stem.ends_with("svc") ||
        stem.ends_with("service")) {
        return AppRole::Helper;
    }
    if (under_windows(path) && !inside(path, "\\systemapps\\")) return AppRole::Tool;
    return AppRole::Application;
}

std::string normalize_app_key(const fs::path& file) {
    return long_path_key(file);
}

InstalledApp describe_executable(const fs::path& file) {
    auto app = make_app({}, file, "manual", AppKind::Manual, "");
    if (!app.description.empty())
        app.name = app.description;
    else
        app.name = path_to_utf8(file.stem());
    app.role = AppRole::Application;
    return app;
}

std::vector<InstalledApp> scan_folder(const fs::path& folder, unsigned depth,
                                      const std::atomic<bool>* cancel) {
    std::vector<InstalledApp> apps;
    std::error_code ec;
    if (!is_local_disk_path(folder) || !fs::is_directory(folder, ec)) return apps;
    std::set<std::string> directories;
    const auto groups = [&] {
        std::vector<fs::path> list{folder};
        for (fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !ec && list.size() < 400; it.increment(ec)) {
            std::error_code type_error;
            if (it->is_directory(type_error)) list.push_back(it->path());
        }
        return list;
    }();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    for (std::size_t i = 0; i < groups.size() && apps.size() < 600; ++i) {
        if (cancelled(cancel) || std::chrono::steady_clock::now() > deadline) break;
        std::vector<Candidate> found;
        if (i == 0) {
            for (fs::directory_iterator it(groups[i], fs::directory_options::skip_permission_denied, ec), end;
                 it != end && !ec; it.increment(ec)) {
                if (lower(it->path().extension().string()) == ".exe" &&
                    classify_executable(it->path(), {}) == AppRole::Application)
                    found.push_back({it->path(), 10});
            }
        } else {
            found = main_executables(groups[i], path_to_utf8(groups[i].filename()), depth > 0 ? depth - 1 : 0,
                                     cancel);
            if (found.size() > 2) found.resize(2);
        }
        for (const auto& candidate : found) {
            auto app = describe_executable(candidate.file);
            app.kind = AppKind::Portable;
            app.source = "folder";
            app.sources = {"folder"};
            if (i != 0 && app.name == path_to_utf8(candidate.file.stem()))
                app.name = path_to_utf8(groups[i].filename());
            apps.push_back(std::move(app));
        }
    }
    return apps;
}

Catalog discover_catalog(const CatalogRequest& request) {
    const bool initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    Collector collector;
    Catalog catalog;
    const auto* cancel = request.cancel;
    const auto run = [&](const char* name, auto&& work) {
        if (cancelled(cancel)) return;
        run_source(catalog.sources, name, collector, request, work);
    };
    run("registry", [&](SourceReport&) { scan_registry(collector, cancel); });
    run("app_paths", [&](SourceReport&) { scan_app_paths(collector); });
    run("start_menu", [&](SourceReport&) { scan_start_menu(collector, cancel); });
    run("store", [&](SourceReport&) { scan_packages(collector, cancel); });
    run("conventional_folders", [&](SourceReport&) { scan_conventional(collector, cancel); });
    run("package_managers", [&](SourceReport&) { scan_package_managers(collector, cancel); });
    run("path", [&](SourceReport&) { scan_path(collector, cancel); });
    run("portable_folders", [&](SourceReport&) {
        scan_roots(collector, request.portable_roots, "portable", AppKind::Portable, cancel);
    });
    run("manual", [&](SourceReport&) {
        for (const auto& file : request.manual) {
            if (!usable_executable(file)) continue;
            auto app = describe_executable(file);
            app.sources = {"manual"};
            app.source = "manual";
            collector.add(std::move(app), Rank::manual, true);
        }
    });
    catalog.cancelled = cancelled(cancel);
    catalog.apps = collector.take();
    std::ranges::sort(catalog.apps, [](const InstalledApp& a, const InstalledApp& b) {
        return lower(a.name) < lower(b.name);
    });
    if (initialized) CoUninitialize();
    return catalog;
}

std::vector<SourceReport> probe_sources() {
    std::vector<SourceReport> reports;
    const auto probe = [&](const char* name, bool available, const char* reason) {
        SourceReport report;
        report.name = name;
        report.available = available;
        if (!available) report.error = reason;
        reports.push_back(std::move(report));
    };
    const auto key_exists = [](HKEY root, const std::wstring& key, REGSAM view = 0) {
        HKEY handle = nullptr;
        const bool ok = RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &handle) == ERROR_SUCCESS;
        if (ok) RegCloseKey(handle);
        return ok;
    };
    std::error_code ec;
    probe("registry",
          key_exists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                     KEY_WOW64_64KEY),
          "the Uninstall key cannot be read");
    probe("app_paths",
          key_exists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths",
                     KEY_WOW64_64KEY),
          "the App Paths key cannot be read");
    probe(
        "start_menu",
        fs::is_directory(environment_path(L"ProgramData") / L"Microsoft\\Windows\\Start Menu\\Programs", ec),
        "the Start menu folder is missing");
    probe(
        "store",
        key_exists(HKEY_LOCAL_MACHINE,
                   L"SOFTWARE\\Classes\\Local "
                   L"Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages"),
        "the package repository is not available on this Windows");
    probe("conventional_folders", fs::is_directory(environment_path(L"ProgramFiles"), ec),
          "Program Files is missing");
    bool managers = false;
    for (const auto& profile : user_profiles())
        managers = managers || fs::is_directory(profile.directory / L"scoop\\apps", ec) ||
                   fs::is_directory(profile.directory / L"AppData\\Local\\Microsoft\\WinGet\\Packages", ec);
    managers = managers || fs::is_directory(environment_path(L"ProgramData") / L"chocolatey\\lib", ec);
    probe("package_managers", managers, "no winget, scoop or chocolatey folders were found");
    probe("path", !path_directories().empty(), "the PATH variable has no usable folders");
    probe("portable_folders", true, "");
    probe("manual", true, "");
    return reports;
}

}  // namespace houseguest::system
