#include "houseguest/core/glob.hpp"

#include <algorithm>
#include <cctype>
#include <span>
#include <vector>

namespace houseguest::core {
namespace {

constexpr std::size_t kMaxDoubleStar = 4;

std::vector<std::string_view> split_segments(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const auto end = s.find('/', start);
        const auto len = (end == std::string_view::npos ? s.size() : end) - start;
        if (len > 0) out.push_back(s.substr(start, len));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

bool is_absolute(std::string_view n) {
    if (n.starts_with("//")) return true;
    return n.size() >= 3 && std::isalpha(static_cast<unsigned char>(n[0])) && n[1] == ':' && n[2] == '/';
}

bool match_segment(std::string_view p, std::string_view s) {
    std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool match_segments(std::span<const std::string_view> p, std::span<const std::string_view> s) {
    if (p.empty()) return s.empty();
    if (p.front() == "**") {
        for (std::size_t skip = 0; skip <= s.size(); ++skip) {
            if (match_segments(p.subspan(1), s.subspan(skip))) return true;
        }
        return false;
    }
    if (s.empty()) return false;
    return match_segment(p.front(), s.front()) && match_segments(p.subspan(1), s.subspan(1));
}

}  // namespace

std::string normalize_path(std::string_view path) {
    std::string out(path);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
    });
    return out;
}

bool is_valid_path_pattern(std::string_view pattern) {
    const auto n = normalize_path(pattern);
    if (!is_absolute(n)) return false;
    const auto segments = split_segments(n);
    return std::ranges::count(segments, std::string_view{"**"}) <=
           static_cast<std::ptrdiff_t>(kMaxDoubleStar);
}

bool path_matches(std::string_view pattern, std::string_view path) {
    if (!is_valid_path_pattern(pattern)) return false;
    const auto np = normalize_path(pattern);
    const auto ns = normalize_path(path);
    if (!is_absolute(ns)) return false;
    const auto ps = split_segments(np);
    const auto ss = split_segments(ns);
    return match_segments(ps, ss);
}

}  // namespace houseguest::core
