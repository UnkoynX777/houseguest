#include "discovery.hpp"

#include <algorithm>

#include "houseguest/core/glob.hpp"
#include "houseguest/core/protection.hpp"
#include "houseguest/core/subject.hpp"

#include "command_support.hpp"

namespace houseguest::service {
namespace {

using Clock = std::chrono::steady_clock;

bool contains_any(const std::string& text, std::initializer_list<const char*> needles) {
    return std::ranges::any_of(needles,
                               [&](const char* needle) { return text.find(needle) != std::string::npos; });
}

std::string directory_of(const std::string& path) {
    const auto normalized = core::normalize_path(path);
    const auto slash = normalized.rfind('/');
    return slash == std::string::npos ? std::string{} : normalized.substr(0, slash);
}

std::string file_name_of(const std::string& path) {
    const auto normalized = core::normalize_path(path);
    const auto slash = normalized.rfind('/');
    return slash == std::string::npos ? normalized : normalized.substr(slash + 1);
}

bool user_writable_location(const std::string& normalized_path) {
    return contains_any(normalized_path,
                        {"/appdata/", "/temp/", "/downloads/", "/desktop/", "/users/public/"});
}

}  // namespace

void Discovery::start(Target target) {
    std::scoped_lock lock(mutex_);
    expire_locked();
    if (running_) throw Failure("conflict", "An observation is already running.");
    if (target.minutes < 1 || target.minutes > 30) throw Failure("bad_request", "Choose 1 to 30 minutes.");
    target_ = std::move(target);
    root_directory_ = directory_of(target_.path);
    started_ = Clock::now();
    ends_ = started_ + std::chrono::minutes(target_.minutes);
    ends_at_ = now_seconds() + target_.minutes * 60;
    items_.clear();
    dropped_ = 0;
    running_ = true;
}

void Discovery::stop() {
    std::scoped_lock lock(mutex_);
    running_ = false;
}

void Discovery::expire_locked() {
    if (running_ && Clock::now() >= ends_) running_ = false;
}

bool Discovery::active() {
    std::scoped_lock lock(mutex_);
    expire_locked();
    return running_;
}

bool Discovery::wants(const std::string& sid) {
    std::scoped_lock lock(mutex_);
    expire_locked();
    return running_ && sid == target_.user_sid;
}

bool Discovery::is_root(const core::AppIdentity& identity) {
    std::scoped_lock lock(mutex_);
    if (!target_.package_family.empty() && !identity.package_family.empty() &&
        core::normalize_path(identity.package_family) == core::normalize_path(target_.package_family)) {
        return true;
    }
    return !target_.path.empty() && core::normalize_path(identity.path) == core::normalize_path(target_.path);
}

bool Discovery::related(const core::AppIdentity& identity) {
    std::scoped_lock lock(mutex_);
    if (!target_.package_family.empty() &&
        core::normalize_path(identity.package_family) == core::normalize_path(target_.package_family)) {
        return true;
    }
    return !root_directory_.empty() && core::normalize_path(identity.path).starts_with(root_directory_ + "/");
}

void Discovery::note(const Observation& observation) {
    std::scoped_lock lock(mutex_);
    expire_locked();
    if (!running_) return;
    const auto key = core::normalize_path(observation.identity.path) + "|" + observation.identity.sha256;
    auto it = items_.find(key);
    if (it == items_.end()) {
        if (items_.size() >= kMaxItems) {
            ++dropped_;
            return;
        }
        Item item;
        item.identity = observation.identity;
        item.parent_path = observation.parent_path;
        item.first_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count();
        item.windows_protected = observation.windows_protected;
        it = items_.emplace(key, std::move(item)).first;
    }
    ++it->second.runs;
    it->second.root |= observation.root;
    it->second.descendant |= observation.descendant || observation.root;
}

core::Json Discovery::classify(const Item& item, const std::string& root_directory,
                               const std::string& root_publisher) const {
    const auto path = core::normalize_path(item.identity.path);
    const auto name = file_name_of(item.identity.path);
    const bool signed_ok = !item.identity.publisher.empty();
    const bool inside = !root_directory.empty() && path.starts_with(root_directory + "/");
    const bool sensitive = core::is_sensitive_tool(item.identity.path);
    const bool same_signer = core::same_subject(item.identity.publisher, root_publisher);

    std::string role = "helper";
    if (item.root)
        role = "main";
    else if (sensitive)
        role = "tool";
    else if (contains_any(name, {"update", "patch"}))
        role = "updater";
    else if (contains_any(path,
                          {"/runtime/", "/jre", "/jdk", "/java", "/node", "/python", "/dotnet", "redist"}) ||
             contains_any(name, {"java", "node.exe", "python", "dotnet"}))
        role = "runtime";
    else if (!item.descendant)
        role = "service";
    else if (contains_any(name, {"launcher", "bootstrap"}))
        role = "launcher";

    std::string verdict;
    if (item.root)
        verdict = "main";
    else if (item.windows_protected)
        verdict = "windows";
    else if (sensitive)
        verdict = "avoid";
    else if (item.descendant && (same_signer || inside || signed_ok))
        verdict = "recommended";
    else
        verdict = "review";

    core::Json risks = core::Json::array();
    if (!signed_ok && !item.windows_protected) risks.push_back("unsigned");
    if (!inside && !item.root) risks.push_back("outside_app_folder");
    if (user_writable_location(path)) risks.push_back("user_writable_location");
    if (sensitive) risks.push_back("sensitive_tool");
    if (!item.descendant) risks.push_back("not_started_by_app");
    if (!same_signer && signed_ok && !item.root) risks.push_back("different_signer");

    return {{"path", item.identity.path},
            {"name", name},
            {"sha256", item.identity.sha256},
            {"publisher", item.identity.publisher},
            {"package_family", item.identity.package_family},
            {"parent", item.parent_path},
            {"runs", item.runs},
            {"first_seen_ms", item.first_ms},
            {"descendant", item.descendant},
            {"role", role},
            {"verdict", verdict},
            {"risks", std::move(risks)}};
}

core::Json Discovery::report() {
    std::scoped_lock lock(mutex_);
    expire_locked();
    std::string root_publisher;
    for (const auto& [key, item] : items_) {
        if (item.root && !item.identity.publisher.empty()) root_publisher = item.identity.publisher;
    }
    std::vector<core::Json> rows;
    for (const auto& [key, item] : items_) rows.push_back(classify(item, root_directory_, root_publisher));
    std::ranges::sort(rows, {},
                      [](const core::Json& row) { return row["first_seen_ms"].get<std::int64_t>(); });
    core::Json items = core::Json::array();
    for (auto& row : rows) items.push_back(std::move(row));
    return {{"running", running_},
            {"label", target_.label},
            {"path", target_.path},
            {"package_family", target_.package_family},
            {"ends_at", ends_at_},
            {"elapsed_ms",
             std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_).count()},
            {"dropped", dropped_},
            {"items", std::move(items)}};
}

}  // namespace houseguest::service
