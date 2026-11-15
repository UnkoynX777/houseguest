#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace houseguest::service::api {

inline constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
inline constexpr std::size_t kMaxBodyBytes = 256 * 1024;
inline constexpr std::size_t kMaxHeaders = 48;

struct Request {
    std::string method;
    std::string target;  // path without the query
    std::string query;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string body;

    [[nodiscard]] std::string_view header(std::string_view name) const;
};

enum class ParseStatus { NeedMore, Complete, Invalid, TooLarge };

struct ParseResult {
    ParseStatus status = ParseStatus::NeedMore;
    std::size_t consumed = 0;
    int http_status = 400;
    std::string reason;
    Request request;
};

// Strict HTTP/1.1 request parser. It rejects chunked bodies, duplicate headers, obsolete line folding, bare
// line feeds and control characters, so a request cannot be read one way by a proxy and another way by us.
[[nodiscard]] ParseResult parse_request(std::string_view buffer);

struct Response {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
};

[[nodiscard]] std::string serialize(const Response& response);
[[nodiscard]] std::string_view reason_phrase(int status);

}  // namespace houseguest::service::api
