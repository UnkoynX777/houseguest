#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace houseguest::core {

using TimePoint = std::chrono::system_clock::time_point;

enum class Action { Allow, Deny };

// All strings are UTF-8. Paths are absolute; the platform layer converts from UTF-16.
struct AppIdentity {
    std::string path;
    std::string sha256;
    std::string publisher;
    std::string package_family;
};

struct PathMatcher {
    std::string pattern;
};
struct HashMatcher {
    std::string sha256;
};
struct PublisherMatcher {
    std::string subject;
};
struct PackageMatcher {
    std::string family_name;
};

using Matcher = std::variant<PathMatcher, HashMatcher, PublisherMatcher, PackageMatcher>;

struct Rule {
    std::string id;
    Action action = Action::Allow;
    Matcher matcher;
    int priority = 0;
    std::optional<TimePoint> expires_at;
};

enum class ComponentRole { Main, Launcher, Helper, Runtime, Updater, Service, Component };

// Descendant components only count while the parent chain reaches an allowed member of the application.
// Independent components are started by Windows or by brokers and need a hash or publisher pin.
enum class Scope { Descendant, Independent };

// SelectedOnly honours the main executables alone. Approved adds the listed components. Related also admits
// unlisted descendants that share the application's signer or install folder, never sensitive tools.
enum class DependencyMode { SelectedOnly, Approved, Related };

// Review denies an unknown descendant and queues it for the owner instead of just dropping it.
enum class UnknownPolicy { Deny, Review };

struct Component {
    std::string id;
    std::string label;
    ComponentRole role = ComponentRole::Main;
    Scope scope = Scope::Descendant;
    std::string path;
    std::string sha256;
    std::string publisher;
    std::string package_family;
    bool follow_updates = false;
    std::optional<TimePoint> expires_at;
};

struct Application {
    std::string id;
    std::string name;
    DependencyMode mode = DependencyMode::Approved;
    UnknownPolicy unknown = UnknownPolicy::Review;
    int priority = 50;
    std::vector<Component> components;
};

struct Profile {
    std::string id;
    std::string name;
    Action default_action = Action::Deny;
    std::vector<Rule> rules;
    std::vector<Application> apps;
};

enum class Continuity { None, Houseguest, WebView2 };

// What the platform layer could establish about a process. The engine never reads the system itself.
struct ProcessFacts {
    AppIdentity app;
    bool image_known = true;
    bool remote_image = false;
    bool hash_resolved = false;
    bool publisher_resolved = false;
    bool kernel_critical = false;
    bool protected_process = false;
    Continuity continuity = Continuity::None;
    std::vector<std::string> lineage;
};

enum class DecisionSource {
    Protected,
    Continuity,
    Rule,
    Application,
    Dependency,
    Related,
    Default,
    Unresolved
};

struct Decision {
    Action action = Action::Deny;
    DecisionSource source = DecisionSource::Default;
    std::string reason;
    std::string rule_id;
    std::string app_id;
    std::string component_id;
    int priority = 0;
    bool needs_hash = false;
    bool needs_publisher = false;
    std::vector<std::string> member_of;
    std::string review_app;
    std::string note;
};

}  // namespace houseguest::core
