#include "http.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace houseguest::service::api {
namespace {

bool token_char(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

ParseResult fail(int status, std::string reason, ParseStatus kind = ParseStatus::Invalid) {
    ParseResult result;
    result.status = kind;
    result.http_status = status;
    result.reason = std::move(reason);
    return result;
}

ParseResult too_large_headers() {
    return fail(431, "Header section is too large.", ParseStatus::TooLarge);
}

}  // namespace

std::string_view Request::header(std::string_view name) const {
    const auto found = headers.find(lower(name));
    return found == headers.end() ? std::string_view{} : std::string_view(found->second);
}

ParseResult parse_request(std::string_view buffer) {
    const auto end = buffer.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        if (buffer.size() > kMaxHeaderBytes) return too_large_headers();
        return {};
    }
    if (end > kMaxHeaderBytes) return too_large_headers();
    const auto head = buffer.substr(0, end);
    for (std::size_t i = 0; i < head.size(); ++i) {
        const auto c = static_cast<unsigned char>(head[i]);
        if (c == '\r' && (i + 1 >= head.size() || head[i + 1] != '\n'))
            return fail(400, "Bare carriage return.");
        if (c == '\n' && (i == 0 || head[i - 1] != '\r')) return fail(400, "Bare line feed.");
        if (c != '\r' && c != '\n' && c != '\t' && (c < 0x20 || c == 0x7f))
            return fail(400, "Control characters.");
    }

    ParseResult result;
    auto& request = result.request;
    std::size_t position = 0;
    bool first = true;
    for (;;) {
        auto stop = head.find("\r\n", position);
        if (stop == std::string_view::npos) stop = head.size();
        const auto line = head.substr(position, stop - position);
        if (first) {
            first = false;
            const auto a = line.find(' ');
            const auto b = a == std::string_view::npos ? a : line.find(' ', a + 1);
            if (b == std::string_view::npos || line.find(' ', b + 1) != std::string_view::npos)
                return fail(400, "Malformed request line.");
            request.method = std::string(line.substr(0, a));
            auto target = line.substr(a + 1, b - a - 1);
            const auto version = line.substr(b + 1);
            if (version != "HTTP/1.1")
                return fail(version.starts_with("HTTP/") ? 505 : 400, "Unsupported version.");
            if (request.method.empty() || !std::ranges::all_of(request.method, token_char))
                return fail(400, "Malformed method.");
            if (target.empty() || target[0] != '/' || target.starts_with("//"))
                return fail(400, "Malformed target.");
            if (const auto q = target.find('?'); q != std::string_view::npos) {
                request.query = std::string(target.substr(q + 1));
                target = target.substr(0, q);
            }
            if (target.find('%') != std::string_view::npos || target.find("..") != std::string_view::npos ||
                target.find('\\') != std::string_view::npos)
                return fail(400, "Encoded or traversing target.");
            request.target = std::string(target);
        } else {
            if (line.empty()) return fail(400, "Empty header line.");
            if (line[0] == ' ' || line[0] == '\t') return fail(400, "Obsolete line folding.");
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || colon == 0) return fail(400, "Malformed header.");
            const auto name = line.substr(0, colon);
            if (!std::ranges::all_of(name, token_char)) return fail(400, "Malformed header name.");
            auto value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
            if (request.headers.size() >= kMaxHeaders) return fail(431, "Too many headers.");
            const auto key = lower(name);
            if (request.headers.contains(key)) return fail(400, "Duplicate header.");
            request.headers.emplace(key, std::string(value));
        }
        if (stop == head.size()) break;
        position = stop + 2;
    }

    if (request.headers.contains("transfer-encoding"))
        return fail(501, "Transfer-Encoding is not supported.");
    if (request.headers.contains("expect")) return fail(417, "Expect is not supported.");
    std::size_t length = 0;
    if (const auto it = request.headers.find("content-length"); it != request.headers.end()) {
        const auto& text = it->second;
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), length);
        if (text.empty() || ec != std::errc{} || ptr != text.data() + text.size())
            return fail(400, "Malformed Content-Length.");
        if (length > kMaxBodyBytes) return fail(413, "Body is too large.", ParseStatus::TooLarge);
    }
    const auto body_start = end + 4;
    if (buffer.size() < body_start + length) return {};
    request.body = std::string(buffer.substr(body_start, length));
    result.status = ParseStatus::Complete;
    result.consumed = body_start + length;
    result.http_status = 200;
    return result;
}

std::string_view reason_phrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 408:
            return "Request Timeout";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 415:
            return "Unsupported Media Type";
        case 417:
            return "Expectation Failed";
        case 429:
            return "Too Many Requests";
        case 431:
            return "Request Header Fields Too Large";
        case 500:
            return "Internal Server Error";
        case 501:
            return "Not Implemented";
        case 503:
            return "Service Unavailable";
        case 505:
            return "HTTP Version Not Supported";
        default:
            return "Error";
    }
}

std::string serialize(const Response& response) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                      std::string(reason_phrase(response.status)) + "\r\n";
    out += "Content-Type: " + response.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    out += "Referrer-Policy: no-referrer\r\n";
    out += "Cross-Origin-Resource-Policy: same-origin\r\n";
    for (const auto& [name, value] : response.headers) {
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "\r\n";
    out += response.body;
    return out;
}

}  // namespace houseguest::service::api
