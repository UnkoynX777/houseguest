#include "houseguest/core/serialization.hpp"

#include <stdexcept>

#include "houseguest/core/protection.hpp"

namespace houseguest::core {
namespace {

constexpr std::size_t kMaxRules = 5000;
constexpr std::size_t kMaxText = 2048;
constexpr std::size_t kMaxApps = 1000;
constexpr std::size_t kMaxComponents = 200;

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

template <class E>
struct Name {
    E value;
    const char* text;
};

constexpr Name<ComponentRole> kRoles[] = {
    {ComponentRole::Main, "main"},          {ComponentRole::Launcher, "launcher"},
    {ComponentRole::Helper, "helper"},      {ComponentRole::Runtime, "runtime"},
    {ComponentRole::Updater, "updater"},    {ComponentRole::Service, "service"},
    {ComponentRole::Component, "component"}};
constexpr Name<Scope> kScopes[] = {{Scope::Descendant, "descendant"}, {Scope::Independent, "independent"}};
constexpr Name<DependencyMode> kModes[] = {{DependencyMode::SelectedOnly, "selected_only"},
                                           {DependencyMode::Approved, "approved"},
                                           {DependencyMode::Related, "related"}};
constexpr Name<UnknownPolicy> kUnknown[] = {{UnknownPolicy::Deny, "deny"}, {UnknownPolicy::Review, "review"}};

template <class E, std::size_t N>
const char* name_of(const Name<E> (&table)[N], E value) {
    for (const auto& item : table)
        if (item.value == value) return item.text;
    return table[0].text;
}

template <class E, std::size_t N>
E value_of(const Name<E> (&table)[N], const std::string& text, const char* what) {
    for (const auto& item : table)
        if (text == item.text) return item.value;
    throw std::invalid_argument(std::string("unknown ") + what);
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

Json to_json(const Component& c) {
    Json json{{"id", c.id},
              {"label", c.label},
              {"role", name_of(kRoles, c.role)},
              {"scope", name_of(kScopes, c.scope)},
              {"path", c.path},
              {"sha256", c.sha256},
              {"publisher", c.publisher},
              {"package_family", c.package_family},
              {"follow_updates", c.follow_updates}};
    if (c.expires_at) json["expires_at"] = to_seconds(*c.expires_at);
    return json;
}

Json to_json(const Application& app) {
    Json components = Json::array();
    for (const auto& c : app.components) components.push_back(to_json(c));
    return {{"id", app.id},
            {"name", app.name},
            {"mode", name_of(kModes, app.mode)},
            {"unknown", name_of(kUnknown, app.unknown)},
            {"priority", app.priority},
            {"components", std::move(components)}};
}

Component component_from_json(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("component must be an object");
    Component c;
    c.id = text(json, "id");
    if (c.id.empty()) throw std::invalid_argument("component id required");
    c.label = text(json, "label", false);
    c.role = value_of(kRoles, text(json, "role", false).empty() ? "main" : text(json, "role"), "role");
    c.scope =
        value_of(kScopes, text(json, "scope", false).empty() ? "descendant" : text(json, "scope"), "scope");
    c.path = text(json, "path", false);
    c.sha256 = text(json, "sha256", false);
    c.publisher = text(json, "publisher", false);
    c.package_family = text(json, "package_family", false);
    c.follow_updates = json.value("follow_updates", false);
    if (const auto it = json.find("expires_at"); it != json.end() && it->is_number_integer())
        c.expires_at = TimePoint{std::chrono::seconds{it->get<std::int64_t>()}};
    return c;
}

Application application_from_json(const Json& json) {
    if (!json.is_object()) throw std::invalid_argument("application must be an object");
    Application app;
    app.id = text(json, "id");
    app.name = text(json, "name");
    if (app.id.empty() || app.name.empty()) throw std::invalid_argument("id and name required");
    app.mode = value_of(kModes, text(json, "mode", false).empty() ? "approved" : text(json, "mode"), "mode");
    app.unknown =
        value_of(kUnknown, text(json, "unknown", false).empty() ? "review" : text(json, "unknown"), "policy");
    app.priority = json.value("priority", 50);
    if (app.priority < -1000 || app.priority > 1000) throw std::invalid_argument("priority out of range");
    if (const auto it = json.find("components"); it != json.end()) {
        if (!it->is_array()) throw std::invalid_argument("components must be an array");
        if (it->size() > kMaxComponents) throw std::invalid_argument("too many components");
        for (const auto& item : *it) app.components.push_back(component_from_json(item));
    }
    return app;
}

Json to_json(const Profile& profile) {
    Json rules = Json::array();
    for (const auto& rule : profile.rules) rules.push_back(to_json(rule));
    Json apps = Json::array();
    for (const auto& app : profile.apps) apps.push_back(to_json(app));
    return {{"id", profile.id},
            {"name", profile.name},
            {"default_action", action_name(profile.default_action)},
            {"rules", std::move(rules)},
            {"apps", std::move(apps)}};
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
    if (const auto apps = json.find("apps"); apps != json.end()) {
        if (!apps->is_array()) throw std::invalid_argument("apps must be an array");
        if (apps->size() > kMaxApps) throw std::invalid_argument("too many applications");
        for (const auto& item : *apps) profile.apps.push_back(application_from_json(item));
    }
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
        for (const auto tool : sensitive_tool_names()) {
            p.rules.push_back(make_rule("deny-" + std::string(tool), Action::Deny,
                                        PathMatcher{"%SystemRoot%\\**\\" + std::string(tool)}, 100));
        }
        return p;
    };

    Profile guest = system_base("builtin-guest", "Guest");
    Profile gaming = system_base("builtin-gaming", "Gaming");
    Profile work = system_base("builtin-work", "Work");
    work.default_action = Action::Allow;
    Profile custom = system_base("builtin-custom", "Custom");
    Profile restricted = system_base("builtin-restricted", "Restricted");
    for (const char* installer : {"setup*.exe", "*installer*.exe", "unins*.exe"}) {
        restricted.rules.push_back(make_rule(std::string("deny-") + installer, Action::Deny,
                                             PathMatcher{std::string("%SystemDrive%\\**\\") + installer},
                                             90));
    }
    return {guest, gaming, work, restricted, custom};
}

}  // namespace houseguest::core
