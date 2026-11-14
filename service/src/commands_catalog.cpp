#include <algorithm>
#include <fstream>

#include "houseguest/system/text.hpp"
#include "houseguest/system/windows_info.hpp"

#include "command_support.hpp"
#include "controller.hpp"

namespace houseguest::service {

using core::Json;

namespace {

constexpr std::size_t kMaxManual = 500;
constexpr std::size_t kMaxRoots = 20;

namespace fs = std::filesystem;

std::string key_of(const std::string& path) {
    return system::normalize_app_key(system::path_from_utf8(path));
}

bool key_under(const std::string& root, const std::string& key) {
    return key.size() > root.size() && key.starts_with(root) && key[root.size()] == '\\';
}

fs::path require_path(const Json& args, const char* name) {
    const auto path = system::path_from_utf8(require_text(args, name));
    if (!path.is_absolute()) throw Failure("bad_request", "Use a full path.");
    if (!system::is_local_disk_path(path))
        throw Failure("bad_request", "Only programs on local disks can be added.");
    return path;
}

fs::path require_executable(const Json& args, const char* name) {
    const auto file = require_path(args, name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) throw Failure("not_found", "That file does not exist.");
    auto extension = file.extension().string();
    std::ranges::transform(extension, extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".exe") throw Failure("bad_request", "Only .exe programs can be added.");
    return file;
}

Json check(const char* step, bool ok, std::string detail) {
    return {{"step", step}, {"ok", ok}, {"detail", std::move(detail)}};
}

}  // namespace

system::CatalogRequest Controller::catalog_request() {
    std::scoped_lock lock(mutex_);
    return catalog_request_locked();
}

system::CatalogRequest Controller::catalog_request_locked() const {
    system::CatalogRequest request;
    for (const auto& root : store_.state.portable_roots)
        request.portable_roots.push_back(system::path_from_utf8(root));
    for (const auto& entry : store_.state.manual_apps)
        request.manual.push_back(system::path_from_utf8(entry.path));
    request.resolutions = store_.state.resolutions;
    request.include_removable = store_.state.include_removable;
    return request;
}

void Controller::register_catalog_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("apps.refresh", Access::Owner, [this](Who, Args args) {
        auto request = catalog_request_locked();
        request.deep_scan = args.value("deep", false);
        const bool started = catalog_.refresh(std::move(request));
        return Json{{"started", started}, {"scanning", true}};
    });

    add("apps.refresh.cancel", Access::Owner, [this](Who, Args) {
        catalog_.request_stop();
        return Json::object();
    });

    add("apps.settings", Access::Owner, [this](Who caller, Args args) {
        if (args.contains("include_removable")) {
            store_.state.include_removable = args.value("include_removable", false);
            store_.save();
            audit_->append(caller.sid, "apps.settings_changed",
                           {{"include_removable", store_.state.include_removable}});
            catalog_.refresh(catalog_request_locked());
        }
        return Json{{"include_removable", store_.state.include_removable}};
    });

    add("apps.manual.add", Access::Owner, [this](Who caller, Args args) {
        std::vector<fs::path> files;
        if (args.contains("paths") && args["paths"].is_array()) {
            for (const auto& item : args["paths"]) {
                if (!item.is_string()) throw Failure("bad_request", "Invalid path list.");
                files.push_back(require_executable(Json{{"path", item}}, "path"));
            }
        } else {
            files.push_back(require_executable(args, "path"));
        }
        std::size_t added = 0;
        for (const auto& file : files) {
            const auto text = system::path_to_utf8(file);
            const auto key = key_of(text);
            if (std::ranges::any_of(store_.state.manual_apps,
                                    [&](const auto& entry) { return key_of(entry.path) == key; }))
                continue;
            if (store_.state.manual_apps.size() >= kMaxManual)
                throw Failure("conflict", "Too many programs were added by hand.");
            store_.state.manual_apps.push_back({text, system::describe_executable(file).name, now_seconds()});
            ++added;
        }
        if (added != 0) store_.save();
        if (added != 0) audit_->append(caller.sid, "apps.manual_added", {{"count", added}});
        catalog_.refresh(catalog_request_locked());
        return Json{{"added", added}};
    });

    add("apps.manual.remove", Access::Owner, [this](Who caller, Args args) {
        const auto key = key_of(require_text(args, "path"));
        const auto removed = std::erase_if(store_.state.manual_apps,
                                           [&](const auto& entry) { return key_of(entry.path) == key; });
        if (removed == 0) throw Failure("not_found", "That program was not added by hand.");
        store_.save();
        audit_->append(caller.sid, "apps.manual_removed", {{"path", require_text(args, "path")}});
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add("apps.manual.relocate", Access::Owner, [this](Who caller, Args args) {
        const auto key = key_of(require_text(args, "path"));
        const auto replacement = require_executable(args, "new_path");
        const auto it = std::ranges::find_if(store_.state.manual_apps,
                                             [&](const auto& entry) { return key_of(entry.path) == key; });
        if (it == store_.state.manual_apps.end())
            throw Failure("not_found", "That program was not added by hand.");
        it->path = system::path_to_utf8(replacement);
        it->name = system::describe_executable(replacement).name;
        store_.save();
        audit_->append(caller.sid, "apps.manual_relocated", {{"path", system::path_to_utf8(replacement)}});
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add("apps.resolve", Access::Owner, [this](Who caller, Args args) {
        const auto id = require_text(args, "id");
        const auto view = catalog_.view();
        const auto entry = std::ranges::find(view.apps, id, &system::InstalledApp::id);
        if (entry == view.apps.end()) throw Failure("not_found", "That library entry no longer exists.");
        if (entry->install_dir.empty())
            throw Failure("bad_request", "This entry has no install folder to choose from.");
        const auto file = require_executable(args, "path");
        const auto root = key_of(entry->install_dir);
        if (!key_under(root, key_of(system::path_to_utf8(file))))
            throw Failure("bad_request", "Choose a program inside the install folder of this entry.");
        store_.state.resolutions[root] = system::path_to_utf8(file);
        store_.save();
        audit_->append(caller.sid, "apps.executable_chosen",
                       {{"entry", entry->name}, {"path", system::path_to_utf8(file)}});
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add("apps.resolve.clear", Access::Owner, [this](Who caller, Args args) {
        const auto id = require_text(args, "id");
        const auto view = catalog_.view();
        const auto entry = std::ranges::find(view.apps, id, &system::InstalledApp::id);
        if (entry == view.apps.end()) throw Failure("not_found", "That library entry no longer exists.");
        if (store_.state.resolutions.erase(key_of(entry->install_dir)) != 0) {
            store_.save();
            audit_->append(caller.sid, "apps.executable_choice_cleared", {{"entry", entry->name}});
        }
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add(
        "apps.folder.scan", Access::Owner,
        [](Who, Args args) {
            const auto folder = require_path(args, "path");
            std::error_code ec;
            if (!fs::is_directory(folder, ec)) throw Failure("not_found", "That folder does not exist.");
            Json list = Json::array();
            for (const auto& app : system::scan_folder(folder)) list.push_back(to_json(app));
            return Json{{"apps", std::move(list)}};
        },
        true);

    add("apps.portable.add", Access::Owner, [this](Who caller, Args args) {
        const auto folder = require_path(args, "path");
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) throw Failure("not_found", "That folder does not exist.");
        const auto text = system::path_to_utf8(folder);
        const auto key = key_of(text);
        if (std::ranges::any_of(store_.state.portable_roots,
                                [&](const auto& root) { return key_of(root) == key; }))
            return Json{{"added", false}};
        if (store_.state.portable_roots.size() >= kMaxRoots)
            throw Failure("conflict", "Too many portable folders.");
        store_.state.portable_roots.push_back(text);
        store_.save();
        audit_->append(caller.sid, "apps.portable_folder_added", {{"path", text}});
        catalog_.refresh(catalog_request_locked());
        return Json{{"added", true}};
    });

    add("apps.portable.remove", Access::Owner, [this](Who caller, Args args) {
        const auto key = key_of(require_text(args, "path"));
        const auto removed =
            std::erase_if(store_.state.portable_roots, [&](const auto& root) { return key_of(root) == key; });
        if (removed == 0) throw Failure("not_found", "That folder is not registered.");
        store_.save();
        audit_->append(caller.sid, "apps.portable_folder_removed", {{"path", require_text(args, "path")}});
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add(
        "apps.sources", Access::Owner,
        [this](Who, Args) {
            bool removable = false;
            {
                std::scoped_lock lock(mutex_);
                removable = store_.state.include_removable;
            }
            const auto view = catalog_.view();
            Json sources = Json::array();
            for (const auto& described : system::describe_sources()) {
                const auto last =
                    std::ranges::find(view.sources, described.name, &system::SourceReport::name);
                Json row = to_json(described);
                row["scanned"] = false;
                if (last != view.sources.end()) {
                    row = to_json(*last);
                    row["scanned"] = true;
                    if (row["note"].get<std::string>().empty()) row["note"] = described.note;
                }
                sources.push_back(std::move(row));
            }
            Json volumes = Json::array();
            for (const auto& volume : system::enumerate_volumes(removable))
                volumes.push_back(to_json(volume));
            Json roots = Json::array();
            Json manual = Json::array();
            {
                std::scoped_lock lock(mutex_);
                for (const auto& root : store_.state.portable_roots) roots.push_back(root);
                for (const auto& entry : store_.state.manual_apps) {
                    std::error_code ec;
                    manual.push_back(
                        {{"path", entry.path},
                         {"name", entry.name},
                         {"missing", !fs::is_regular_file(system::path_from_utf8(entry.path), ec)}});
                }
            }
            std::size_t resolved = 0, candidates = 0, unresolved = 0, offline = 0;
            for (const auto& app : view.apps) {
                if (app.resolution == system::Resolution::Resolved)
                    ++resolved;
                else if (app.resolution == system::Resolution::Candidates)
                    ++candidates;
                else
                    ++unresolved;
                if (app.offline) ++offline;
            }
            return Json{{"sources", std::move(sources)},
                        {"volumes", std::move(volumes)},
                        {"portable_roots", std::move(roots)},
                        {"manual", std::move(manual)},
                        {"scanning", view.scanning},
                        {"deep", view.deep},
                        {"updated_at", view.updated_at},
                        {"scan_ms", view.scan_ms},
                        {"total", view.apps.size()},
                        {"resolved", resolved},
                        {"candidates", candidates},
                        {"unresolved", unresolved},
                        {"offline", offline},
                        {"discarded", view.discarded.size()},
                        {"include_removable", removable}};
        },
        true);

    add(
        "apps.diagnose", Access::Owner,
        [this](Who, Args args) {
            const auto raw = require_text(args, "path");
            const auto file = system::path_from_utf8(raw);
            Json checks = Json::array();
            const auto verdict = [&](const char* code, const std::string& advice) {
                return Json{{"verdict", code}, {"advice", advice}, {"checks", checks}};
            };
            if (!file.is_absolute()) {
                checks.push_back(check("path", false, "The path is not absolute."));
                return verdict("invalid_path",
                               "Enter the full path, for example C:\\Program Files\\App\\app.exe.");
            }
            std::error_code ec;
            if (fs::is_directory(file, ec)) {
                checks.push_back(check("path", true, "This is a folder."));
                const auto view = catalog_.view();
                const auto key = key_of(raw);
                const auto inside = std::ranges::find_if(view.apps, [&](const system::InstalledApp& app) {
                    return !app.install_dir.empty() &&
                           (key_of(app.install_dir) == key || key_under(key, key_of(app.install_dir)));
                });
                if (inside != view.apps.end()) {
                    checks.push_back(check("catalog", true,
                                           "It holds or equals the install folder of " + inside->name + "."));
                    auto result = verdict("folder_known", "This folder already belongs to a library entry.");
                    result["app"] = to_json(*inside);
                    return result;
                }
                return verdict("folder",
                               "Use Add by folder to look for programs inside it, or run a deep scan.");
            }
            const bool exists = fs::is_regular_file(file, ec);
            const auto view = catalog_.view();
            const auto volume = system::enumerate_volumes(true);
            const auto drive = file.string().substr(0, 2);
            const auto info = std::ranges::find_if(
                volume, [&](const system::VolumeInfo& v) { return v.root.substr(0, 2) == drive; });
            checks.push_back(
                check("exists", exists, exists ? "The file exists." : "The file does not exist."));
            if (!exists) {
                if (info != volume.end() && !info->ready) {
                    checks.push_back(
                        check("volume", false,
                              "The volume " + drive +
                                  " is not ready, so its programs are kept but shown as offline."));
                    return verdict("offline_volume", "Reconnect the drive and refresh the library.");
                }
                return verdict("not_found", "Check the spelling or reinstall the program.");
            }
            auto extension = file.extension().string();
            std::ranges::transform(extension, extension.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            checks.push_back(
                check("executable", extension == ".exe",
                      extension == ".exe" ? "It is an .exe program." : "Only .exe programs are listed."));
            if (extension != ".exe") return verdict("not_executable", "Select the program's .exe file.");
            const bool local = system::is_local_disk_path(file);
            checks.push_back(check("local_disk", local,
                                   local ? "It is on a local disk."
                                         : "It is on a network location, which Houseguest never opens."));
            if (!local) return verdict("network", "Copy the program to a local disk to manage it.");
            if (info != volume.end() && !info->included) {
                checks.push_back(check("volume", false, info->reason));
                return verdict("volume_skipped",
                               "Enable removable drives in the library settings to include this volume.");
            }
            std::ifstream probe(file, std::ios::binary);
            checks.push_back(
                check("readable", probe.good(),
                      probe.good() ? "Houseguest can read it." : "Windows denied access to this file."));
            if (!probe.good())
                return verdict("access_denied", "Allow the service to read this folder or move the program.");

            const auto key = system::normalize_app_key(file);
            const auto described = system::describe_executable(file);
            const bool metadata =
                !described.company.empty() || !described.version.empty() || !described.description.empty();
            checks.push_back(check("metadata", metadata,
                                   metadata
                                       ? "It has version information."
                                       : "It has no version information, so it shows only its file name."));

            const auto exact = std::ranges::find_if(view.apps, [&](const system::InstalledApp& app) {
                return !app.path.empty() && key_of(app.path) == key;
            });
            checks.push_back(check("catalog", exact != view.apps.end(),
                                   exact != view.apps.end() ? "It is the program of a library entry."
                                                            : "It is not the program of any library entry."));
            if (exact != view.apps.end()) {
                Json result = verdict("listed", "");
                result["app"] = to_json(*exact);
                std::string note =
                    "Found by " + std::to_string(exact->sources.size()) + " source(s) and shown once.";
                if (exact->role != system::AppRole::Application) {
                    result["verdict"] = "filtered";
                    result["advice"] =
                        "It is listed as a tool or helper and hidden by default. Turn on Tools and helpers "
                        "to see it.";
                    note = std::string("It was classified as ") + system::role_name(exact->role) + ".";
                }
                result["checks"].push_back(check("sources", true, note));
                return result;
            }
            for (const auto& app : view.apps) {
                const auto candidate = std::ranges::find_if(
                    app.candidates,
                    [&](const system::ExecutableCandidate& c) { return key_of(c.path) == key; });
                if (candidate == app.candidates.end()) continue;
                std::string why;
                for (const auto& reason : candidate->reasons) why += (why.empty() ? "" : ", ") + reason;
                checks.push_back(check("candidate", true,
                                       "It is a candidate main program of " + app.name +
                                           (why.empty() ? "" : " (" + why + ")") + "."));
                auto result =
                    verdict(app.resolution == system::Resolution::Candidates ? "candidate" : "component",
                            app.resolution == system::Resolution::Candidates
                                ? "Houseguest could not tell which program is the main one. Choose it in the "
                                  "library entry."
                                : "It belongs to " + app.name +
                                      ", whose main program was identified as another file.");
                result["app"] = to_json(app);
                return result;
            }
            for (const auto& app : view.apps) {
                if (app.install_dir.empty() || !key_under(key_of(app.install_dir), key)) continue;
                checks.push_back(
                    check("component", true, "It lies inside the install folder of " + app.name + "."));
                auto result = verdict("component", "It is a component of " + app.name +
                                                       ". Add it by hand if you need it on its own.");
                result["app"] = to_json(app);
                return result;
            }
            for (const auto& item : view.discarded) {
                if (key_of(item.path) != key) continue;
                checks.push_back(check("discarded", false,
                                       "It was set aside: " + item.reason + " (" + item.source + ")."));
                return verdict("discarded", "If this is a real program, add it by hand.");
            }
            if (view.scanning)
                return verdict("scanning", "The library is still being refreshed. Try again in a moment.");
            checks.push_back(check("sources", false,
                                   "No registry entry, shortcut, package, game-platform manifest or "
                                   "registered folder points to this file."));
            return verdict("not_detected",
                           "Add it with Add application, register its folder as portable, or run a deep scan "
                           "of the drives.");
        },
        true);
}

}  // namespace houseguest::service
