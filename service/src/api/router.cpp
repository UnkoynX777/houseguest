#include "router.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "../command_support.hpp"

namespace houseguest::service::api {
namespace {

using core::Json;

// The only commands reachable through the API. Everything that changes credentials, disables protection,
// restores state or reaches outside the policy store is absent on purpose.
constexpr Route kRoutes[] = {
    {"GET", "/v1/status", "status", Risk::Read},
    {"GET", "/v1/events", "events.wait", Risk::Read},
    {"GET", "/v1/dashboard", "dashboard", Risk::Read},
    {"GET", "/v1/profiles", "profiles.list", Risk::Read},
    {"GET", "/v1/users", "users.list", Risk::Read},
    {"GET", "/v1/apps", "library.list", Risk::Read},
    {"GET", "/v1/blocks", "blocks.list", Risk::Read},
    {"GET", "/v1/history", "audit.list", Risk::Read},
    {"GET", "/v1/diagnostics", "diagnostics", Risk::Read},
    {"GET", "/v1/reviews", "reviews.list", Risk::Read},
    {"POST", "/v1/reviews/approve", "reviews.approve", Risk::Sensitive},
    {"POST", "/v1/reviews/dismiss", "reviews.dismiss", Risk::Change},
    {"POST", "/v1/profiles", "profiles.save", Risk::Sensitive},
    {"POST", "/v1/session/start", "session.start", Risk::Sensitive},
    {"POST", "/v1/session/stop", "session.stop", Risk::Change},
    {"POST", "/v1/session/extend", "session.extend", Risk::Change},
};

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string decode(std::string_view in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

Json query_args(const std::string& query) {
    Json args = Json::object();
    std::size_t position = 0;
    int count = 0;
    while (position < query.size() && count++ < 16) {
        auto stop = query.find('&', position);
        if (stop == std::string::npos) stop = query.size();
        const auto pair = std::string_view(query).substr(position, stop - position);
        position = stop + 1;
        const auto eq = pair.find('=');
        const auto key = decode(pair.substr(0, eq));
        const auto value = eq == std::string_view::npos ? std::string() : decode(pair.substr(eq + 1));
        if (key.empty() || key.size() > 32) continue;
        std::int64_t number = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (!value.empty() && value.size() < 16 && ec == std::errc{} && ptr == value.data() + value.size())
            args[key] = number;
        else if (value == "true" || value == "false")
            args[key] = value == "true";
        else
            args[key] = value.substr(0, 256);
    }
    return args;
}

int status_for(const std::string& code) {
    if (code == "unauthorized") return 401;
    if (code == "forbidden") return 403;
    if (code == "not_found") return 404;
    if (code == "conflict") return 409;
    if (code == "limit" || code == "rate_limited") return 429;
    if (code == "bad_request" || code == "invalid") return 400;
    return 400;
}

bool nonce_shape(const std::string& nonce) {
    return nonce.size() >= 16 && nonce.size() <= 64 &&
           std::ranges::all_of(nonce, [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

}  // namespace

Router::Router(RouterConfig config) : config_(std::move(config)) {}

Response Router::reply(int status, const Json& body, const Request& request) const {
    Response response;
    response.status = status;
    response.body = body.dump();
    cors(response, request);
    return response;
}

Response Router::failure(int status, const std::string& code, const std::string& message,
                         const Request& request) const {
    return reply(status, Json{{"ok", false}, {"error", {{"code", code}, {"message", message}}}}, request);
}

void Router::cors(Response& response, const Request& request) const {
    const auto origin = std::string(request.header("origin"));
    if (!origin.empty() && config_.origins.contains(origin)) {
        response.headers["Access-Control-Allow-Origin"] = origin;
        response.headers["Vary"] = "Origin";
        response.headers["Access-Control-Allow-Headers"] = "Authorization, Content-Type, X-HG-Nonce, X-HG-Timestamp";
        response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS";
        response.headers["Access-Control-Max-Age"] = "600";
    }
}

bool Router::fresh_nonce(const std::string& nonce, std::int64_t now) {
    std::scoped_lock lock(mutex_);
    while (!nonce_order_.empty() && (nonce_order_.front().first < now - 2 * kSkewSeconds || nonce_order_.size() > 4096)) {
        nonces_.erase(nonce_order_.front().second);
        nonce_order_.pop_front();
    }
    if (!nonces_.insert(nonce).second) return false;
    nonce_order_.emplace_back(now, nonce);
    return true;
}

bool Router::take_device_slot(const std::string& id) {
    std::scoped_lock lock(mutex_);
    const auto [it, inserted] = buckets_.try_emplace(id, 10.0, 20.0);
    return it->second.take();
}

bool Router::take_failure_slot() {
    std::scoped_lock lock(mutex_);
    return failures_.take();
}

Response Router::handle(const Request& request) {
    const auto now = config_.clock ? config_.clock() : now_seconds();

    if (!config_.hosts.contains(lower(std::string(request.header("host")))))
        return failure(403, "host_not_allowed", "Unexpected Host header.", request);

    const auto origin = std::string(request.header("origin"));
    if (!origin.empty() && !config_.origins.contains(origin))
        return failure(403, "origin_not_allowed", "This origin may not use the API.", request);

    if (request.method == "OPTIONS") {
        if (origin.empty()) return failure(405, "method_not_allowed", "Method not allowed.", request);
        Response response;
        response.status = 204;
        cors(response, request);
        return response;
    }

    if (config_.precheck) {
        if (const auto refusal = config_.precheck(request)) {
            if (config_.audit) config_.audit("api", "api.denied", {{"reason", *refusal}, {"path", request.target}});
            return failure(403, "identity_required", *refusal, request);
        }
    }

    if (request.method == "GET" && request.target == "/v1/health")
        return reply(200, Json{{"ok", true}, {"data", config_.health ? config_.health() : Json::object()}}, request);

    if (request.method == "GET") {
        for (const auto& asset : config_.assets) {
            if (request.target != asset.route) continue;
            Response page;
            page.content_type = std::string(asset.content_type) + "; charset=utf-8";
            page.body = std::string(asset.body);
            page.headers["Content-Security-Policy"] =
                "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; "
                "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
            page.headers["X-Frame-Options"] = "DENY";
            page.headers["Permissions-Policy"] = "camera=(), microphone=(), geolocation=()";
            return page;
        }
    }

    const auto header = std::string(request.header("authorization"));
    std::optional<Device> device;
    if (header.starts_with("Bearer ") && config_.devices) device = config_.devices->authenticate(header.substr(7), now);
    if (!device) {
        if (!take_failure_slot()) return failure(429, "rate_limited", "Too many failed attempts.", request);
        return failure(401, "unauthorized", "A valid device token is required.", request);
    }
    if (!take_device_slot(device->id)) return failure(429, "rate_limited", "Too many requests.", request);

    const Route* route = nullptr;
    bool path_known = false;
    for (const auto& candidate : kRoutes) {
        if (request.target != candidate.path) continue;
        path_known = true;
        if (request.method == candidate.method) route = &candidate;
    }
    const bool is_stepup = request.method == "POST" && request.target == "/v1/stepup";
    const bool is_devices = request.method == "GET" && request.target == "/v1/devices";
    const bool is_logout = request.method == "POST" && request.target == "/v1/devices/self/revoke";
    if (!route && !is_stepup && !is_devices && !is_logout)
        return path_known || request.target.starts_with("/v1/")
                   ? failure(path_known ? 405 : 404, path_known ? "method_not_allowed" : "not_found",
                             "No such operation.", request)
                   : failure(404, "not_found", "No such operation.", request);

    const bool mutating = request.method != "GET";
    Json body = Json::object();
    if (mutating) {
        if (!request.body.empty() && !lower(std::string(request.header("content-type"))).starts_with("application/json"))
            return failure(415, "unsupported_media_type", "Send application/json.", request);
        const auto timestamp = std::string(request.header("x-hg-timestamp"));
        const auto nonce = std::string(request.header("x-hg-nonce"));
        std::int64_t sent = 0;
        const auto [ptr, ec] = std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), sent);
        if (timestamp.empty() || ec != std::errc{} || ptr != timestamp.data() + timestamp.size() ||
            std::abs(now - sent) > kSkewSeconds || !nonce_shape(nonce))
            return failure(400, "replay_protection", "A fresh X-HG-Timestamp and X-HG-Nonce are required.", request);
        if (!fresh_nonce(device->id + ":" + nonce, now))
            return failure(409, "replayed", "This request was already used.", request);
        if (!request.body.empty()) {
            if (!nesting_within(request.body, kMaxJsonDepth))
                return failure(400, "bad_request", "The body is nested too deeply.", request);
            body = Json::parse(request.body, nullptr, false);
            if (!body.is_object()) return failure(400, "bad_request", "The body must be a JSON object.", request);
        }
    }

    if (is_devices) {
        Json rows = Json::array();
        for (const auto& d : config_.devices->list())
            rows.push_back({{"id", d.id}, {"name", d.name}, {"scope", scope_name(d.scope)}, {"created", d.created},
                            {"expires", d.expires}, {"last_seen", d.last_seen}, {"revoked", d.revoked},
                            {"current", d.id == device->id}});
        return reply(200, Json{{"ok", true}, {"data", rows}}, request);
    }
    if (is_logout) {
        config_.devices->revoke(device->id);
        if (config_.audit) config_.audit("device:" + device->id, "api.device.signed_out", {{"name", device->name}});
        return reply(200, Json{{"ok", true}, {"data", Json::object()}}, request);
    }
    if (is_stepup) {
        if (device->scope != Scope::Write)
            return failure(403, "read_only_device", "This device can only read.", request);
        {
            std::scoped_lock lock(mutex_);
            auto& [until, count] = pin_failures_[device->id];
            if (until > now) return failure(429, "rate_limited", "Wait before trying again.", request);
        }
        const auto pin = body.is_object() && body.contains("pin") && body["pin"].is_string()
                             ? body["pin"].get<std::string>()
                             : std::string();
        const bool ok = !pin.empty() && config_.pin_check && config_.pin_check(pin);
        std::scoped_lock lock(mutex_);
        auto& [until, count] = pin_failures_[device->id];
        if (!ok) {
            if (++count >= 5) {
                until = now + 60 * std::min(count - 4, 30);
            }
            if (config_.audit) config_.audit("device:" + device->id, "api.stepup.denied", {{"name", device->name}});
            return failure(401, "unauthorized", "The PIN is not correct.", request);
        }
        count = 0;
        until = 0;
        stepup_[device->id] = now + kStepupSeconds;
        if (config_.audit) config_.audit("device:" + device->id, "api.stepup", {{"name", device->name}});
        return reply(200, Json{{"ok", true}, {"data", {{"expires_in", kStepupSeconds}}}}, request);
    }

    if (route->risk != Risk::Read && device->scope != Scope::Write)
        return failure(403, "read_only_device", "This device can only read.", request);
    if (route->risk == Risk::Sensitive) {
        std::scoped_lock lock(mutex_);
        const auto it = stepup_.find(device->id);
        if (it == stepup_.end() || it->second < now)
            return failure(403, "stepup_required", "Confirm with the owner PIN first.", request);
    }

    Json args = mutating ? body : query_args(request.query);
    try {
        auto data = config_.gateway(route->command, args, *device);
        if (route->risk != Risk::Read && config_.audit)
            config_.audit("device:" + device->id, std::string("api.") + route->command, {{"name", device->name}});
        return reply(200, Json{{"ok", true}, {"data", std::move(data)}}, request);
    } catch (const Failure& e) {
        return failure(status_for(e.code), e.code, e.what(), request);
    } catch (const std::exception&) {
        return failure(400, "bad_request", "The request could not be processed.", request);
    }
}

}  // namespace houseguest::service::api
