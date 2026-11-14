#include "houseguest/core/serialization.hpp"

#include <stdexcept>

namespace houseguest::core {
namespace {

constexpr std::size_t kMaxRules = 5000;
constexpr std::size_t kMaxText = 2048;

std::string text(const Json& json, const char* key, bool required = true) {
    const auto it = json.find(key);
    if (it == json.end()) {
        if (required) throw std::invalid_argument(std::string("missing field: ") + key);
        return {};
    }
    if (!it->is_string()) throw std::invalid_argument(std::string("field must be text: ") + key);
    auto value = it->get<std::string>();
    if (value.size() > kMaxText) throw std::invalid_argument(std::string("field too long: ") + key);
    return value;
}

Action action_from(const std::string& value) {
    if (value == "allow") return Action::Allow;
    if (value == "deny") return Action::Deny;
    throw std::invalid_argument("action must be allow or deny");
}

const char* action_name(Action action) {
    return action == Action::Allow ? "allow" : "deny";
}

std::int64_t to_seconds(TimePoint t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

Rule make_rule(std::string id, Action action, Matcher matcher, int priority) {
    return Rule{std::move(id), action, std::move(matcher), priority, std::nullopt};
}

}  // namespace

Json to_json(const Rule& rule) {
    Json json{{"id", rule.id}, {"action", action_name(rule.action)}, {"priority", rule.priority}};
    std::visit(
        [&](const auto& m) {
            using T = std::decay_t<decltype(m)>;
            if constexpr (std::is_same_v<T, PathMatcher>) {
                json["match"] = {{"type", "path"}, {"value", m.pattern}};
            } else if constexpr (std::is_same_v<T, HashMatcher>) {
                json["match"] = {{"type", "hash"}, {"value", m.sha256}};
            } else if constexpr (std::is_same_v<T, PublisherMatcher>) {
                json["match"] = {{"type", "publisher"}, {"value", m.subject}};
            } else {
                json["match"] = {{"type", "package"}, {"value", m.family_name}};
            }
        },
        rule.matcher);
    if (rule.expires_at) json["expires_at"] = to_seconds(*rule.expires_at);
    return json;
}

Rule rule_from_json(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("rule must be an object");
    const auto match = json.find("match");
    if (match == json.end() || !match->is_object()) throw std::invalid_argument("missing match");
    const auto type = text(*match, "type");
    const auto value = text(*match, "value");
    Matcher matcher;
    if (type == "path")
        matcher = PathMatcher{value};
    else if (type == "hash")
        matcher = HashMatcher{value};
    else if (type == "publisher")
        matcher = PublisherMatcher{value};
    else if (type == "package")
        matcher = PackageMatcher{value};
    else
        throw std::invalid_argument("unknown match type");

    Rule rule = make_rule(text(json, "id"), action_from(text(json, "action")), std::move(matcher),
                          json.value("priority", 0));
    if (rule.priority < -1000 || rule.priority > 1000) {
        throw std::invalid_argument("priority out of range");
    }
    if (const auto it = json.find("expires_at"); it != json.end() && it->is_number_integer()) {
        rule.expires_at = TimePoint{std::chrono::seconds{it->get<std::int64_t>()}};
    }
    return rule;
}

Json to_json(const Profile& profile) {
    Json rules = Json::array();
    for (const auto& rule : profile.rules) rules.push_back(to_json(rule));
    return {{"id", profile.id},
            {"name", profile.name},
            {"default_action", action_name(profile.default_action)},
            {"rules", std::move(rules)}};
}

Profile profile_from_json(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("profile must be an object");
    Profile profile;
    profile.id = text(json, "id");
    profile.name = text(json, "name");
    if (profile.id.empty() || profile.name.empty()) throw std::invalid_argument("id and name required");
    profile.default_action = action_from(text(json, "default_action"));
    const auto it = json.find("rules");
    if (it == json.end() || !it->is_array()) throw std::invalid_argument("rules must be an array");
    if (it->size() > kMaxRules) throw std::invalid_argument("too many rules");
    for (const auto& item : *it) profile.rules.push_back(rule_from_json(item));
    return profile;
}

Json to_json(const AppIdentity& app) {
    return {{"path", app.path},
            {"sha256", app.sha256},
            {"publisher", app.publisher},
            {"package_family", app.package_family}};
}

AppIdentity app_identity_from_json(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("app must be an object");
    return {text(json, "path"), text(json, "sha256", false), text(json, "publisher", false),
            text(json, "package_family", false)};
}

std::vector<Profile> builtin_profiles() {
    constexpr const char* kWindowsPublisher =
        "CN=Microsoft Windows, O=Microsoft Corporation, L=Redmond, S=Washington, C=US";
    const auto system_base = [](std::string id, std::string name) {
        Profile p{std::move(id), std::move(name), Action::Deny, {}};
        p.rules.push_back(
            make_rule("windows-components", Action::Allow, PathMatcher{"%SystemRoot%\\SystemApps\\**"}, 10));
        p.rules.push_back(
            make_rule("windows-signed", Action::Allow, PublisherMatcher{kWindowsPublisher}, 10));
        constexpr const char* kAdminTools[] = {
            "cmd.exe",     "powershell.exe",     "pwsh.exe",    "regedit.exe", "mmc.exe",
            "taskmgr.exe", "msiexec.exe",        "wscript.exe", "cscript.exe", "mshta.exe",
            "control.exe", "systemsettings.exe", "wt.exe",      "eventvwr.exe"};
        for (const char* tool : kAdminTools) {
            p.rules.push_back(make_rule(std::string("deny-") + tool, Action::Deny,
                                        PathMatcher{std::string("%SystemRoot%\\**\\") + tool}, 100));
        }
        return p;
    };

    Profile guest = system_base("builtin-guest", "Guest");
    Profile gaming = system_base("builtin-gaming", "Gaming");
    Profile work = system_base("builtin-work", "Work");
    work.default_action = Action::Allow;
    Profile restricted = system_base("builtin-restricted", "Restricted");
    for (const char* installer : {"setup*.exe", "*installer*.exe", "unins*.exe"}) {
        restricted.rules.push_back(make_rule(std::string("deny-") + installer, Action::Deny,
                                             PathMatcher{std::string("%SystemDrive%\\**\\") + installer},
                                             90));
    }
    return {guest, gaming, work, restricted};
}

}  // namespace houseguest::core
