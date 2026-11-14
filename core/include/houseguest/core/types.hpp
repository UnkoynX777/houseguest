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

struct Profile {
    std::string id;
    std::string name;
    Action default_action = Action::Deny;
    std::vector<Rule> rules;
};

enum class DecisionSource { Baseline, Rule, Default };

struct Decision {
    Action action = Action::Deny;
    DecisionSource source = DecisionSource::Default;
    std::string rule_id;
    int priority = 0;
};

}  // namespace houseguest::core
