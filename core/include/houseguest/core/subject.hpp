#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace houseguest::core {

using SubjectField = std::pair<std::string, std::string>;

// Splits an X.500 subject such as "C=US, O=Contoso, CN=Contoso Ltd" into lower-case attribute names and
// values. Windows writes the attributes in the order they are encoded in the certificate, people write them
// the other way round, so every comparison goes through this form.
[[nodiscard]] std::vector<SubjectField> parse_subject(std::string_view subject);

// True when both subjects name the same attributes with the same values, in any order. Empty never matches.
[[nodiscard]] bool same_subject(std::string_view a, std::string_view b);

[[nodiscard]] bool subject_has(std::string_view subject, std::string_view name, std::string_view value);

}  // namespace houseguest::core
