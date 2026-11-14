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
    return request;
}

void Controller::register_catalog_commands() {
    using Args = const Json&;
    using Who = const Caller&;

    add("apps.refresh", Access::Owner, [this](Who, Args) {
        const bool started = catalog_.refresh(catalog_request_locked());
        return Json{{"started", started}, {"scanning", true}};
    });

    add("apps.refresh.cancel", Access::Owner, [this](Who, Args) {
        catalog_.request_stop();
        return Json::object();
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
        {
            for (const auto& file : files) {
                const auto text = system::path_to_utf8(file);
                const auto key = key_of(text);
                if (std::ranges::any_of(store_.state.manual_apps,
                                        [&](const auto& entry) { return key_of(entry.path) == key; }))
                    continue;
                if (store_.state.manual_apps.size() >= kMaxManual)
                    throw Failure("conflict", "Too many programs were added by hand.");
                store_.state.manual_apps.push_back(
                    {text, system::describe_executable(file).name, now_seconds()});
                ++added;
            }
            if (added != 0) store_.save();
        }
        if (added != 0) audit_->append(caller.sid, "apps.manual_added", {{"count", added}});
        catalog_.refresh(catalog_request_locked());
        return Json{{"added", added}};
    });

    add("apps.manual.remove", Access::Owner, [this](Who caller, Args args) {
        const auto key = key_of(require_text(args, "path"));
        {
            const auto removed = std::erase_if(store_.state.manual_apps,
                                               [&](const auto& entry) { return key_of(entry.path) == key; });
            if (removed == 0) throw Failure("not_found", "That program was not added by hand.");
            store_.save();
        }
        audit_->append(caller.sid, "apps.manual_removed", {{"path", require_text(args, "path")}});
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add("apps.manual.relocate", Access::Owner, [this](Who caller, Args args) {
        const auto key = key_of(require_text(args, "path"));
        const auto replacement = require_executable(args, "new_path");
        {
            const auto it = std::ranges::find_if(
                store_.state.manual_apps, [&](const auto& entry) { return key_of(entry.path) == key; });
            if (it == store_.state.manual_apps.end())
                throw Failure("not_found", "That program was not added by hand.");
            it->path = system::path_to_utf8(replacement);
            it->name = system::describe_executable(replacement).name;
            store_.save();
        }
        audit_->append(caller.sid, "apps.manual_relocated", {{"path", system::path_to_utf8(replacement)}});
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
            for (const auto& app : system::scan_folder(folder, 3)) list.push_back(to_json(app));
            return Json{{"apps", std::move(list)}};
        },
        true);

    add("apps.portable.add", Access::Owner, [this](Who caller, Args args) {
        const auto folder = require_path(args, "path");
        std::error_code ec;
        if (!fs::is_directory(folder, ec)) throw Failure("not_found", "That folder does not exist.");
        const auto text = system::path_to_utf8(folder);
        {
            const auto key = key_of(text);
            if (std::ranges::any_of(store_.state.portable_roots,
                                    [&](const auto& root) { return key_of(root) == key; }))
                return Json{{"added", false}};
            if (store_.state.portable_roots.size() >= kMaxRoots)
                throw Failure("conflict", "Too many portable folders.");
            store_.state.portable_roots.push_back(text);
            store_.save();
        }
        audit_->append(caller.sid, "apps.portable_folder_added", {{"path", text}});
        catalog_.refresh(catalog_request_locked());
        return Json{{"added", true}};
    });

    add("apps.portable.remove", Access::Owner, [this](Who caller, Args args) {
        const auto key = key_of(require_text(args, "path"));
        {
            const auto removed = std::erase_if(store_.state.portable_roots,
                                               [&](const auto& root) { return key_of(root) == key; });
            if (removed == 0) throw Failure("not_found", "That folder is not registered.");
            store_.save();
        }
        audit_->append(caller.sid, "apps.portable_folder_removed", {{"path", require_text(args, "path")}});
        catalog_.refresh(catalog_request_locked());
        return Json::object();
    });

    add(
        "apps.sources", Access::Owner,
        [this](Who, Args) {
            const auto view = catalog_.view();
            Json sources = Json::array();
            for (const auto& probe : system::probe_sources()) {
                const auto last = std::ranges::find(view.sources, probe.name, &system::SourceReport::name);
                Json row{{"name", probe.name},    {"available", probe.available},
                         {"reason", probe.error}, {"found", 0},
                         {"milliseconds", 0},     {"scanned", false}};
                if (last != view.sources.end()) {
                    row["found"] = last->found;
                    row["milliseconds"] = last->milliseconds;
                    row["scanned"] = true;
                    if (!last->error.empty()) row["reason"] = last->error;
                    if (!last->error.empty()) row["available"] = false;
                }
                sources.push_back(std::move(row));
            }
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
            return Json{{"sources", std::move(sources)}, {"portable_roots", std::move(roots)},
                        {"manual", std::move(manual)},   {"scanning", view.scanning},
                        {"updated_at", view.updated_at}, {"total", view.apps.size()}};
        },
        true);

    add(
        "apps.diagnose", Access::Owner,
        [this](Who, Args args) {
            const auto raw = require_text(args, "path");
            const auto file = system::path_from_utf8(raw);
            Json checks = Json::array();
            const auto verdict = [&](const char* code, const char* advice) {
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
                return verdict("folder", "Use Add by folder to look for programs inside it.");
            }
            const bool exists = fs::is_regular_file(file, ec);
            checks.push_back(
                check("exists", exists, exists ? "The file exists." : "The file does not exist."));
            if (!exists) return verdict("not_found", "Check the spelling or reinstall the program.");
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
            std::ifstream probe(file, std::ios::binary);
            checks.push_back(
                check("readable", probe.good(),
                      probe.good() ? "Houseguest can read it." : "Windows denied access to this file."));
            if (!probe.good())
                return verdict("access_denied", "Allow the service to read this folder or move the program.");

            const auto view = catalog_.view();
            const auto key = system::normalize_app_key(file);
            const auto found = std::ranges::find_if(view.apps, [&](const system::InstalledApp& app) {
                return system::normalize_app_key(system::path_from_utf8(app.path)) == key;
            });
            const auto info = system::describe_executable(file);
            const bool metadata = !info.company.empty() || !info.version.empty() || !info.description.empty();
            checks.push_back(check("metadata", metadata,
                                   metadata
                                       ? "It has version information."
                                       : "It has no version information, so it shows only its file name."));
            checks.push_back(
                check("catalog", found != view.apps.end(),
                      found != view.apps.end() ? "It is in the library." : "It is not in the library."));
            if (found != view.apps.end()) {
                Json result = verdict("listed", "");
                result["app"] = to_json(*found);
                std::string note =
                    "Found by " + std::to_string(found->sources.size()) + " source(s) and shown once.";
                if (found->role != system::AppRole::Application) {
                    result["verdict"] = "filtered";
                    result["advice"] =
                        "It is listed as a tool or helper and hidden by default. Turn on Tools and helpers "
                        "to see it.";
                    note = std::string("It was classified as ") + system::role_name(found->role) + ".";
                }
                result["checks"].push_back(check("sources", true, note));
                return result;
            }
            const auto folder = key.substr(0, key.find_last_of('\\'));
            for (const auto& other : view.apps) {
                const auto other_key = system::normalize_app_key(system::path_from_utf8(other.path));
                if (other_key.substr(0, other_key.find_last_of('\\')) == folder) {
                    checks.push_back(check("same_folder", true,
                                           "Another program from the same folder is listed: " + other.name));
                    return verdict("sibling_listed",
                                   "This file is probably a component of the listed program. Add it by hand "
                                   "if you need it on its own.");
                }
            }
            if (view.scanning)
                return verdict("scanning", "The library is still being refreshed. Try again in a moment.");
            checks.push_back(check(
                "sources", false,
                "No registry entry, App Paths entry, Start menu shortcut or package points to this file."));
            return verdict("not_detected",
                           "Add it with Add application. For many programs in one place, use Add by folder "
                           "or register a portable folder.");
        },
        true);
}

}  // namespace houseguest::service
