#include "api.hpp"

#include <regex>

#include "houseguest/system/crypto.hpp"

namespace houseguest::service::cloudflare {
namespace {

using core::Json;
constexpr const char* kBase = "https://api.cloudflare.com/client/v4";

std::string message_of(const Json& doc, int status) {
    if (doc.is_object() && doc.contains("errors") && doc["errors"].is_array() && !doc["errors"].empty()) {
        const auto& first = doc["errors"][0];
        if (first.is_object() && first.contains("message") && first["message"].is_string())
            return first["message"].get<std::string>();
    }
    if (status == 401 || status == 403) return "Cloudflare refused the API token or it lacks a permission.";
    return "Cloudflare answered with status " + std::to_string(status) + ".";
}

std::string standard_base64(const system::Bytes& data) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        std::uint32_t v = static_cast<std::uint32_t>(data[i]) << 16;
        if (i + 1 < data.size()) v |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        if (i + 2 < data.size()) v |= data[i + 2];
        out += table[(v >> 18) & 63];
        out += table[(v >> 12) & 63];
        out += i + 1 < data.size() ? table[(v >> 6) & 63] : '=';
        out += i + 2 < data.size() ? table[v & 63] : '=';
    }
    return out;
}

}  // namespace

bool valid_hostname(const std::string& hostname) {
    static const std::regex pattern(
        R"(^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$)");
    return hostname.size() <= 253 && std::regex_match(hostname, pattern);
}

Api::Api(Transport transport, std::string token) : transport_(std::move(transport)), token_(std::move(token)) {}

Json Api::call(const std::string& method, const std::string& path, const Json& body) {
    static const std::regex safe(R"(^/[A-Za-z0-9/_?=&.-]+$)");
    if (!std::regex_match(path, safe) || path.find("..") != std::string::npos || path.find("//") != std::string::npos)
        throw ApiError("An identifier contained characters that are not allowed.");
    system::HttpsRequest request;
    request.method = method;
    request.url = std::string(kBase) + path;
    request.headers = {{"Authorization", "Bearer " + token_}, {"Accept", "application/json"}};
    if (!body.is_null()) {
        request.headers.emplace_back("Content-Type", "application/json");
        request.body = body.dump();
    }
    const auto response = transport_(request);
    if (response.status == 0)
        throw ApiError(response.error.empty() ? "Cloudflare could not be reached." : response.error);
    const Json doc = Json::parse(response.body, nullptr, false);
    if (!response.ok() || !doc.is_object() || doc.value("success", false) != true)
        throw ApiError(message_of(doc, response.status), response.status);
    return doc.contains("result") ? doc["result"] : Json::object();
}

Json Api::verify_token() { return call("GET", "/user/tokens/verify"); }
Json Api::accounts() { return call("GET", "/accounts?per_page=20"); }
Json Api::zones() { return call("GET", "/zones?per_page=50&status=active"); }

std::string Api::access_domain(const std::string& account_id) {
    const auto org = call("GET", "/accounts/" + account_id + "/access/organizations");
    if (!org.is_object() || !org.contains("auth_domain") || !org["auth_domain"].is_string())
        throw ApiError("Cloudflare Zero Trust is not set up for this account yet.");
    return org["auth_domain"].get<std::string>();
}

TunnelCredentials Api::create_tunnel(const std::string& account_id, const std::string& name) {
    const auto secret = standard_base64(system::random_bytes(32));
    const auto result = call("POST", "/accounts/" + account_id + "/cfd_tunnel",
                             {{"name", name}, {"tunnel_secret", secret}, {"config_src", "local"}});
    if (!result.contains("id") || !result["id"].is_string()) throw ApiError("Cloudflare did not return a tunnel.");
    return {account_id, result["id"].get<std::string>(), secret};
}

void Api::delete_tunnel(const std::string& account_id, const std::string& tunnel_id) {
    call("DELETE", "/accounts/" + account_id + "/cfd_tunnel/" + tunnel_id + "/connections");
    call("DELETE", "/accounts/" + account_id + "/cfd_tunnel/" + tunnel_id);
}

void Api::create_dns_record(const std::string& zone_id, const std::string& hostname, const std::string& tunnel_id) {
    if (!valid_hostname(hostname)) throw ApiError("The hostname is not valid.");
    call("POST", "/zones/" + zone_id + "/dns_records",
         {{"type", "CNAME"},
          {"name", hostname},
          {"content", tunnel_id + ".cfargotunnel.com"},
          {"proxied", true},
          {"comment", "Houseguest remote access"}});
}

void Api::delete_dns_record(const std::string& zone_id, const std::string& hostname) {
    const auto found = call("GET", "/zones/" + zone_id + "/dns_records?type=CNAME&name=" + hostname);
    if (!found.is_array()) return;
    for (const auto& record : found)
        if (record.contains("id") && record["id"].is_string() &&
            record.value("comment", "") == "Houseguest remote access")
            call("DELETE", "/zones/" + zone_id + "/dns_records/" + record["id"].get<std::string>());
}

AccessApp Api::create_access_app(const std::string& account_id, const std::string& name, const std::string& hostname,
                                 const std::vector<std::string>& emails) {
    if (!valid_hostname(hostname)) throw ApiError("The hostname is not valid.");
    if (emails.empty()) throw ApiError("At least one e-mail address must be allowed.");
    Json include = Json::array();
    for (const auto& email : emails) include.push_back({{"email", {{"email", email}}}});
    const auto policy = call("POST", "/accounts/" + account_id + "/access/policies",
                             {{"name", name + " owner"}, {"decision", "allow"}, {"include", include}});
    if (!policy.contains("id") || !policy["id"].is_string()) throw ApiError("Cloudflare did not return a policy.");
    const auto app = call("POST", "/accounts/" + account_id + "/access/apps",
                          {{"name", name},
                           {"domain", hostname},
                           {"type", "self_hosted"},
                           {"session_duration", "1h"},
                           {"app_launcher_visible", false},
                           {"policies", {{{"id", policy["id"]}, {"precedence", 1}}}}});
    if (!app.contains("id") || !app.contains("aud") || !app["aud"].is_string())
        throw ApiError("Cloudflare did not return an Access application.");
    return {app["id"].get<std::string>(), app["aud"].get<std::string>()};
}

void Api::delete_access_app(const std::string& account_id, const std::string& app_id) {
    call("DELETE", "/accounts/" + account_id + "/access/apps/" + app_id);
}

Json Api::tunnel_status(const std::string& account_id, const std::string& tunnel_id) {
    return call("GET", "/accounts/" + account_id + "/cfd_tunnel/" + tunnel_id);
}

}  // namespace houseguest::service::cloudflare
