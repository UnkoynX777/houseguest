#include "houseguest/core/subject.hpp"

#include <algorithm>
#include <cctype>

namespace houseguest::core {
namespace {

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return text;
}

std::string unquoted(std::string_view value) {
    value = trimmed(value);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return std::string(value);
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '"' && i + 2 < value.size() && value[i + 1] == '"') ++i;
        out += value[i];
    }
    return out;
}

}  // namespace

std::vector<SubjectField> parse_subject(std::string_view subject) {
    std::vector<SubjectField> fields;
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i <= subject.size(); ++i) {
        if (i < subject.size() && subject[i] == '"') quoted = !quoted;
        if (i < subject.size() && (subject[i] != ',' || quoted)) continue;
        const auto part = trimmed(subject.substr(start, i - start));
        start = i + 1;
        const auto equals = part.find('=');
        if (equals == std::string_view::npos || equals == 0) continue;
        fields.emplace_back(lowered(trimmed(part.substr(0, equals))),
                            lowered(unquoted(part.substr(equals + 1))));
    }
    std::ranges::sort(fields);
    return fields;
}

bool same_subject(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return false;
    const auto left = parse_subject(a);
    const auto right = parse_subject(b);
    if (left.empty() || right.empty()) return lowered(trimmed(a)) == lowered(trimmed(b));
    return left == right;
}

bool subject_has(std::string_view subject, std::string_view name, std::string_view value) {
    const SubjectField wanted{lowered(name), lowered(value)};
    const auto fields = parse_subject(subject);
    return std::ranges::find(fields, wanted) != fields.end();
}

}  // namespace houseguest::core
